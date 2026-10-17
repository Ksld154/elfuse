/*
 * PI pthread mutex handoff and robust owner death
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests:
 *   1. Three threads contend one PTHREAD_PRIO_INHERIT mutex. FUTEX_UNLOCK_PI
 *      must leave FUTEX_WAITERS set while a waiter stays queued; a release to
 *      0 lets the woken thread take the word with a guest CAS, its own unlock
 *      then makes no syscall, and the third waiter parks forever.
 *
 *   2. A robust PI mutex whose owner exits holding it: the next lock returns
 *      EOWNERDEAD. The robust list tags PI entries in bit 0, and the takeover
 *      must keep FUTEX_OWNER_DIED in the word for libc to report it.
 *
 *   3. Same, with the waiter already parked in a timed lock (10 s deadline)
 *      when the owner exits. It must return EOWNERDEAD well inside the
 *      deadline rather than wait it out.
 *
 * Syscalls exercised: futex(98), set_robust_list(99), clone(220)
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define CONTEND_THREADS 3
#define CONTEND_ITERS 20000
#define CONTEND_BUDGET_SEC 30

static pthread_mutex_t contend_mutex;
static volatile long contend_counter;
static int contend_done;

static void *contend_worker(void *arg)
{
    (void) arg;
    for (int i = 0; i < CONTEND_ITERS; i++) {
        pthread_mutex_lock(&contend_mutex);
        contend_counter++;
        pthread_mutex_unlock(&contend_mutex);
    }
    __atomic_add_fetch(&contend_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

/* pthread_create reports its error as the return value, not errno. */
static int spawn(pthread_t *th, void *fn(void *))
{
    int rc = pthread_create(th, NULL, fn, NULL);
    if (rc != 0) {
        errno = rc;
        FAIL("pthread_create");
    }
    return rc;
}

static void test_contend(void)
{
    TEST("PI mutex, 3 contending threads");

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&contend_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_t th[CONTEND_THREADS];
    for (int i = 0; i < CONTEND_THREADS; i++) {
        if (spawn(&th[i], contend_worker) != 0)
            exit(1);
    }

    /* A parked waiter nobody wakes never returns, so poll instead of joining;
     * on a hang the process exits with the workers still parked.
     */
    double start = mono_now();
    while (__atomic_load_n(&contend_done, __ATOMIC_SEQ_CST) < CONTEND_THREADS) {
        if (mono_now() - start > CONTEND_BUDGET_SEC) {
            printf("FAIL: %d of %d threads finished in %d s (lost wakeup)\n",
                   __atomic_load_n(&contend_done, __ATOMIC_SEQ_CST),
                   CONTEND_THREADS, CONTEND_BUDGET_SEC);
            fails++;
            SUMMARY("test-futex-pi-mutex");
            fflush(stdout);
            _exit(1);
        }
        usleep(10000);
    }
    for (int i = 0; i < CONTEND_THREADS; i++)
        pthread_join(th[i], NULL);

    EXPECT_EQ(contend_counter, (long) CONTEND_THREADS * CONTEND_ITERS,
              "counter mismatch");
}

static pthread_mutex_t robust_mutex;
static int owner_locked;
static int owner_hold_ms;

static void *robust_owner(void *arg)
{
    (void) arg;
    pthread_mutex_lock(&robust_mutex);
    __atomic_store_n(&owner_locked, 1, __ATOMIC_SEQ_CST);
    if (owner_hold_ms)
        usleep((useconds_t) owner_hold_ms * 1000);
    return NULL; /* Exits holding the mutex. */
}

static void robust_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&robust_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    owner_locked = 0;
}

static int timedlock_in(int sec)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += sec;
    return pthread_mutex_timedlock(&robust_mutex, &dl);
}

static void robust_recover(int rc)
{
    if (rc == EOWNERDEAD) {
        pthread_mutex_consistent(&robust_mutex);
        pthread_mutex_unlock(&robust_mutex);
    } else if (rc == 0) {
        pthread_mutex_unlock(&robust_mutex);
    }
}

static void test_robust_dead_owner(void)
{
    TEST("robust PI owner exits, then lock");

    robust_init();
    owner_hold_ms = 0;
    pthread_t th;
    if (spawn(&th, robust_owner) != 0)
        return;
    pthread_join(th, NULL);

    /* timedlock, not lock: on ESRCH glibc parks pthread_mutex_lock forever. */
    int rc = timedlock_in(5);
    if (rc != EOWNERDEAD)
        printf("FAIL: expected EOWNERDEAD(%d), got %d\n", EOWNERDEAD, rc);
    if (rc == EOWNERDEAD)
        PASS();
    else
        fails++;
    robust_recover(rc);
}

static void test_robust_timed_waiter(void)
{
    TEST("robust PI owner exits under waiter");

    robust_init();
    owner_hold_ms = 300;
    pthread_t th;
    if (spawn(&th, robust_owner) != 0)
        return;
    while (!__atomic_load_n(&owner_locked, __ATOMIC_SEQ_CST))
        usleep(1000);

    double start = mono_now();
    int rc = timedlock_in(10);
    double took = mono_now() - start;
    pthread_join(th, NULL);

    if (rc != EOWNERDEAD || took >= 2.0) {
        printf("FAIL: expected EOWNERDEAD(%d) in < 2 s, got %d after %.2f s\n",
               EOWNERDEAD, rc, took);
        fails++;
    } else {
        PASS();
    }
    robust_recover(rc);
}

int main(void)
{
    printf("test-futex-pi-mutex\n");
    test_contend();
    test_robust_dead_owner();
    test_robust_timed_waiter();
    SUMMARY("test-futex-pi-mutex");
    return fails == 0 ? 0 : 1;
}

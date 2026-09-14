/*
 * A wait's temporary sigmask stays in force until its signal is delivered
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ppoll, pselect6 and epoll_pwait install the caller's sigmask for the wait
 * only. When a signal that mask unblocks ends the wait, Linux delivers it
 * before putting the original mask back: the handler runs under the temporary
 * mask, and rt_sigreturn restores the original. A thread that blocks the signal
 * everywhere else and unblocks it only inside the wait -- the reason these
 * calls take a mask at all -- must therefore still see its handler run, rather
 * than EINTR with the signal left pending under the restored mask.
 */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

enum wait_kind { WAIT_PPOLL, WAIT_PSELECT, WAIT_EPOLL };

static _Atomic int handler_runs;
static _Atomic int handler_saw_usr2_blocked;

static void on_signal(int sig)
{
    (void) sig;
    sigset_t cur;
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    if (sigismember(&cur, SIGUSR2))
        atomic_store_explicit(&handler_saw_usr2_blocked, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&handler_runs, 1, memory_order_relaxed);
}

static void *killer(void *arg)
{
    (void) arg;
    usleep(100000);
    kill(getpid(), SIGUSR1);
    return NULL;
}

/* One wait on @rfd under @mask with no timeout. */
static int wait_masked(enum wait_kind kind,
                       int rfd,
                       int epfd,
                       const sigset_t *mask)
{
    switch (kind) {
    case WAIT_PPOLL: {
        struct pollfd p = {.fd = rfd, .events = POLLIN};
        return ppoll(&p, 1, NULL, mask);
    }
    case WAIT_PSELECT: {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(rfd, &set);
        return pselect(rfd + 1, &set, NULL, NULL, NULL, mask);
    }
    case WAIT_EPOLL: {
        struct epoll_event ev;
        return epoll_pwait(epfd, &ev, 1, -1, mask);
    }
    }
    return -1;
}

/* True when the calling thread's mask is exactly the test's original one:
 * SIGUSR1 blocked, SIGUSR2 not.
 */
static bool mask_is_original(void)
{
    sigset_t cur;
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    return sigismember(&cur, SIGUSR1) && !sigismember(&cur, SIGUSR2);
}

static void check_kind(enum wait_kind kind, const char *name)
{
    char label[64];
    int p[2];
    if (pipe(p) < 0) {
        TEST(name);
        FAIL("pipe failed");
        return;
    }
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN};
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev) < 0) {
        TEST(name);
        FAIL("epoll setup failed");
        return;
    }

    /* The wait unblocks SIGUSR1 and blocks SIGUSR2, the reverse of the thread's
     * own mask, so both halves of the swap are visible.
     */
    sigset_t wait_mask;
    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, SIGUSR2);

    snprintf(label, sizeof(label), "%s: handler runs", name);
    TEST(label);
    atomic_store_explicit(&handler_runs, 0, memory_order_relaxed);
    atomic_store_explicit(&handler_saw_usr2_blocked, 0, memory_order_relaxed);
    pthread_t th;
    int rc = pthread_create(&th, NULL, killer, NULL);
    if (rc != 0) {
        errno = rc;
        FAIL("pthread_create failed");
        return;
    }
    int ret = wait_masked(kind, p[0], epfd, &wait_mask);
    int saved = errno;
    pthread_join(th, NULL);
    int runs = atomic_load_explicit(&handler_runs, memory_order_acquire);
    if (ret == -1 && saved == EINTR && runs == 1) {
        PASS();
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "ret=%d errno=%d handler ran %d times", ret,
                 saved, runs);
        FAIL(msg);
    }

    snprintf(label, sizeof(label), "%s: handler mask", name);
    TEST(label);
    if (atomic_load_explicit(&handler_saw_usr2_blocked, memory_order_acquire))
        PASS();
    else
        FAIL("handler did not run under the wait's mask");

    snprintf(label, sizeof(label), "%s: mask after EINTR", name);
    TEST(label);
    if (mask_is_original())
        PASS();
    else
        FAIL("original mask not restored");

    /* A wait that returns ready has no signal to deliver, so the original mask
     * has to be back before the call returns.
     */
    snprintf(label, sizeof(label), "%s: mask after ready", name);
    TEST(label);
    if (write(p[1], "x", 1) != 1) {
        FAIL("write failed");
    } else {
        ret = wait_masked(kind, p[0], epfd, &wait_mask);
        if (ret == 1 && mask_is_original())
            PASS();
        else
            FAIL("ready wait did not restore the original mask");
    }

    close(epfd);
    close(p[0]);
    close(p[1]);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGUSR1, &sa, NULL);

    /* Blocked before the killer exists, so no thread but a waiter under the
     * temporary mask can take it.
     */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    check_kind(WAIT_PPOLL, "ppoll");
    check_kind(WAIT_PSELECT, "pselect6");
    check_kind(WAIT_EPOLL, "epoll_pwait");

    SUMMARY("test-wait-sigmask-signal");
    return fails ? 1 : 0;
}

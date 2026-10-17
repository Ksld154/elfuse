/*
 * SysV semaphore tests: GETALL/SETALL across set sizes, blocking semop, and
 * EIDRM for a semop whose set is removed while it waits.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

int passes = 0, fails = 0;

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

static void test_getall_setall(int nsems)
{
    char name[48];
    snprintf(name, sizeof(name), "GETALL/SETALL nsems=%d", nsems);
    TEST(name);

    int id = semget(IPC_PRIVATE, nsems, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("semget");
        return;
    }

    unsigned short *in = calloc((size_t) nsems, sizeof(*in));
    unsigned short *out = calloc((size_t) nsems, sizeof(*out));
    if (!in || !out) {
        FAIL("calloc");
        goto out;
    }
    for (int i = 0; i < nsems; i++)
        in[i] = (unsigned short) ((i * 7 + 3) % 1000);

    union semun arg = {.array = in};
    if (semctl(id, 0, SETALL, arg) < 0) {
        FAIL("SETALL");
        goto out;
    }
    arg.array = out;
    if (semctl(id, 0, GETALL, arg) < 0) {
        FAIL("GETALL");
        goto out;
    }
    if (memcmp(in, out, (size_t) nsems * sizeof(*in)) != 0) {
        FAIL("GETALL mismatch");
        goto out;
    }
    int last = semctl(id, nsems - 1, GETVAL);
    if (last != in[nsems - 1]) {
        FAIL("GETVAL mismatch");
        goto out;
    }
    PASS();
out:
    free(in);
    free(out);
    semctl(id, 0, IPC_RMID);
}

/* A mixed set whose IPC_NOWAIT operation is the blocker must refuse with
 * EAGAIN; this is the path that reads the values through GETALL.
 */
static void test_semop_nowait(int nsems)
{
    char name[48];
    snprintf(name, sizeof(name), "semop NOWAIT blocker n=%d", nsems);
    TEST(name);

    int id = semget(IPC_PRIVATE, nsems, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("semget");
        return;
    }
    struct sembuf ops[2] = {
        {.sem_num = 0, .sem_op = 1, .sem_flg = 0},
        {.sem_num = (unsigned short) (nsems - 1),
         .sem_op = -1,
         .sem_flg = IPC_NOWAIT},
    };
    if (nsems == 1)
        ops[1].sem_op = -2;
    EXPECT_ERRNO(semop(id, ops, 2), EAGAIN, "expected EAGAIN");
    semctl(id, 0, IPC_RMID);
}

/* The waiter runs in a forked child rather than a thread: a pthread test needs
 * its own link rule, and the host semantics are the same across processes. The
 * child exits 0 on success, 1 on the expected error, 2 on anything else.
 */
static pid_t spawn_semop_waiter(int id)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct sembuf op = {.sem_num = 0, .sem_op = -1, .sem_flg = 0};
        if (semop(id, &op, 1) == 0)
            _exit(0);
        _exit(errno == EIDRM ? 1 : 2);
    }
    return pid;
}

static void test_semop_blocks_then_wakes(void)
{
    TEST("semop blocks until posted");
    int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("semget");
        return;
    }
    pid_t pid = spawn_semop_waiter(id);
    sleep_ms(200);
    int st;
    pid_t early = waitpid(pid, &st, WNOHANG);
    struct sembuf post = {.sem_num = 0, .sem_op = 1, .sem_flg = 0};
    semop(id, &post, 1);
    int rc = early == 0 ? reap_within(pid, 5000) : -1;
    if (early != 0)
        FAIL("semop returned before post");
    else if (rc != 0)
        FAIL("semop after post");
    else
        PASS();
    semctl(id, 0, IPC_RMID);
}

static void test_semop_eidrm(void)
{
    TEST("semop sees EIDRM on RMID");
    int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("semget");
        return;
    }
    pid_t pid = spawn_semop_waiter(id);
    sleep_ms(200);
    semctl(id, 0, IPC_RMID);
    int rc = reap_within(pid, 5000);
    if (rc == 1)
        PASS();
    else
        FAIL("expected EIDRM");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("test-sysv-sem\n");

    static const int sizes[] = {1, 16, 1024, 1025, 4096};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        test_getall_setall(sizes[i]);
    test_semop_nowait(1);
    test_semop_nowait(1025);
    test_semop_blocks_then_wakes();
    test_semop_eidrm();

    SUMMARY("test-sysv-sem");
    return fails ? 1 : 0;
}

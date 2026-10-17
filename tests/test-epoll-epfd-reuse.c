/*
 * epoll fd close+reuse race regression test
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * sys_epoll_ctl pins the host kqueue behind epfd and the epoll instance behind
 * epfd in two separate fd_lock windows. A sibling that closes epfd and creates
 * a new epoll instance on the same number between the two used to pair the old
 * kqueue with the new instance: the ADD armed its knote on the dying kqueue but
 * marked the registration active on the new instance, so the registration never
 * fired there and a retry answered EEXIST.
 *
 * The invariant checked after every race round: if the epoll fd on that number
 * refuses a second ADD with EEXIST, it has to report the always-readable pipe.
 * Either outcome of the race is legal on its own (the ADD landing on the old
 * instance, or on the new one); only the mixture is not.
 *
 * The window is a few host calls wide, so the main thread sweeps a short delay
 * before its close to walk across it. A clean run is not a proof; the test is a
 * stress lane, not a deterministic reproducer.
 *
 * Syscalls exercised: epoll_create1(20), epoll_ctl(21), epoll_pwait(22),
 *                     pipe2(59), write(64), close(57), clone(220), futex(98),
 *                     exit(93)
 */

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "test-harness.h"
#include "raw-syscall.h"

int passes = 0, fails = 0;

#define ROUNDS 4000

static _Atomic int go_round;
static _Atomic int done_round;
static _Atomic int adds_won;
static int race_epfd;
static int pipe_rd;
static char adder_stack[16384] __attribute__((aligned(16)));

/* CLONE_THREAD sibling with no libc TLS, so raw syscalls only. */
static void adder(void)
{
    for (int round = 1; round <= ROUNDS; round++) {
        while (atomic_load_explicit(&go_round, memory_order_acquire) != round)
            ;
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = pipe_rd};
        if (raw_syscall4(__NR_epoll_ctl, race_epfd, EPOLL_CTL_ADD, pipe_rd,
                         (long) &ev) == 0)
            atomic_fetch_add_explicit(&adds_won, 1, memory_order_relaxed);
        atomic_store_explicit(&done_round, round, memory_order_release);
    }
    raw_syscall1(__NR_exit, 0);
}

int main(void)
{
    printf("test-epoll-epfd-reuse\n");

    int p[2];
    if (pipe(p) != 0 || write(p[1], "x", 1) != 1) {
        perror("pipe");
        return 1;
    }
    pipe_rd = p[0];

    /* CLONE_VM | FS | FILES | SIGHAND | THREAD | CHILD_CLEARTID */
    long flags = 0x00000100 | 0x00000200 | 0x00000400 | 0x00000800 |
                 0x00010000 | 0x00200000;
    volatile int child_tid = 1;
    long ret = raw_syscall5(__NR_clone, flags,
                            (long) (adder_stack + sizeof(adder_stack)), 0, 0,
                            (long) &child_tid);
    if (ret == 0) {
        adder();
        return 0; /* unreachable */
    }
    if (ret < 0) {
        printf("clone failed: %ld\n", ret);
        return 1;
    }

    int mixed = 0, reused = 0;
    for (int round = 1; round <= ROUNDS; round++) {
        race_epfd = epoll_create1(0);
        if (race_epfd < 0) {
            perror("epoll_create1");
            return 1;
        }
        atomic_store_explicit(&go_round, round, memory_order_release);

        for (volatile int spin = 0; spin < (round % 64) * 8; spin++)
            ;
        close(race_epfd);
        int fresh = epoll_create1(0);

        while (atomic_load_explicit(&done_round, memory_order_acquire) != round)
            ;
        if (fresh != race_epfd) {
            if (fresh >= 0)
                close(fresh);
            continue;
        }
        reused++;

        struct epoll_event out;
        int n = epoll_wait(fresh, &out, 1, 0);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = pipe_rd};
        int r = epoll_ctl(fresh, EPOLL_CTL_ADD, pipe_rd, &ev);
        if (r == -1 && errno == EEXIST && n == 0)
            mixed++;
        close(fresh);
    }
    raw_wait_cleartid(&child_tid);

    TEST("epfd number reused each round");
    EXPECT_TRUE(reused > ROUNDS / 2, "epoll_create1 did not reuse the number");

    /* Every ADD losing to the close would leave nothing to split. */
    TEST("racing ADD succeeded in some round");
    EXPECT_TRUE(atomic_load_explicit(&adds_won, memory_order_relaxed) > 0,
                "every racing ADD failed");

    TEST("no ADD split across instances");
    if (mixed == 0)
        PASS();
    else {
        printf("(%d rounds) ", mixed);
        FAIL("registration active on the new instance but never fires");
    }

    SUMMARY("test-epoll-epfd-reuse");
    return fails ? 1 : 0;
}

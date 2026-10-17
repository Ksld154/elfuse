/*
 * Test that a pidfd survives as long as any alias of it
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * dup(2) and F_DUPFD give the new fd the same open file description, so closing
 * the original must not change what the alias reports: it stays unreadable
 * while the target lives, it still signals the target, and waitid(P_PIDFD) on
 * it still reaps the target. A pidfd number closed and reused by an unrelated
 * file must stop naming the target.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linux-pidfd.h"
#include "test-harness.h"

int passes = 0, fails = 0;

static int poll_now(int fd)
{
    struct pollfd pf = {.fd = fd, .events = POLLIN};
    return poll(&pf, 1, 0);
}

int main(void)
{
    printf("test-pidfd-dup: pidfd aliases\n");

    int gate[2];
    if (pipe(gate) < 0) {
        perror("pipe");
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        char c;
        close(gate[1]);
        (void) read(gate[0], &c, 1);
        _exit(7);
    }
    close(gate[0]);

    int pfd = pidfd_open(child);
    TEST("pidfd_open(child)");
    EXPECT_TRUE(pfd >= 0, "pidfd_open failed");

    int d = dup(pfd);
    TEST("dup(pidfd)");
    EXPECT_TRUE(d >= 0, "dup failed");
    close(pfd);

    TEST("dup alias not ready");
    EXPECT_TRUE(poll_now(d) == 0, "alias readable while target lives");

    TEST("pidfd_send_signal(dup, 0)");
    EXPECT_TRUE(pidfd_send_signal(d, 0) == 0, "send_signal on alias failed");

    int d2 = fcntl(d, F_DUPFD, 20);
    TEST("F_DUPFD(alias)");
    EXPECT_TRUE(d2 >= 20, "F_DUPFD failed");
    close(d);

    TEST("F_DUPFD alias not ready");
    EXPECT_TRUE(poll_now(d2) == 0, "F_DUPFD alias readable");

    TEST("pidfd_send_signal(F_DUPFD, 0)");
    EXPECT_TRUE(pidfd_send_signal(d2, 0) == 0, "send_signal failed");

    /* Release the child and wait for its exit through the alias. */
    close(gate[1]);
    struct pollfd pf = {.fd = d2, .events = POLLIN};
    TEST("alias reports exit");
    EXPECT_TRUE(poll(&pf, 1, 10000) == 1 && (pf.revents & POLLIN),
                "no exit on alias");

    siginfo_t si;
    memset(&si, 0, sizeof(si));
    TEST("waitid(P_PIDFD, alias)");
    EXPECT_TRUE(waitid(P_PIDFD, (id_t) d2, &si, WEXITED) == 0 &&
                    si.si_pid == child && si.si_status == 7,
                "waitid on alias failed");
    close(d2);

    /* A reused number must not reach the old target. */
    int self = pidfd_open(getpid());
    TEST("pidfd_open(self)");
    EXPECT_TRUE(self >= 0, "pidfd_open(self) failed");
    close(self);
    int reuse = open("/dev/null", O_RDONLY);
    TEST("reused number is not a pidfd");
    EXPECT_TRUE(
        reuse == self && pidfd_send_signal(reuse, 0) == -1 && errno == EBADF,
        "reused number still signals");
    close(reuse);

    SUMMARY("test-pidfd-dup");
    return fails ? 1 : 0;
}

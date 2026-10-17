/*
 * Test that a pidfd survives fork
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * fork(2) gives the child every open file description the parent had, pidfds
 * included. The inherited pidfd names the same target: it signals it, it turns
 * readable when the target exits, and a dup alias of it stays an alias. The
 * child is not the target's parent, so waitid(P_PIDFD) on it reports ECHILD, as
 * Linux does for any non-child. Each forked checker prints its own lines and
 * exits with its failure count.
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

static volatile sig_atomic_t got_usr1;

static void on_usr1(int sig)
{
    (void) sig;
    got_usr1 = 1;
}

static int poll_in(int fd, int timeout_ms)
{
    struct pollfd pf = {.fd = fd, .events = POLLIN};
    int n = poll(&pf, 1, timeout_ms);
    if (n == 1 && !(pf.revents & POLLIN))
        return -1;
    return n;
}

static int waitid_echild(int fd)
{
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    return waitid(P_PIDFD, (id_t) fd, &si, WEXITED | WNOHANG) == -1 &&
           errno == ECHILD;
}

static int reap_ok(pid_t pid)
{
    int status;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/* Target is a sibling the checker cannot wait on. */
static void test_sibling_target(void)
{
    int gate[2];
    if (pipe(gate) < 0) {
        perror("pipe");
        fails++;
        return;
    }
    pid_t target = fork();
    if (target < 0) {
        perror("fork");
        fails++;
        return;
    }
    if (target == 0) {
        char c;
        close(gate[1]);
        (void) read(gate[0], &c, 1);
        _exit(0);
    }
    close(gate[0]);

    int pfd = pidfd_open(target);
    int alias = pfd >= 0 ? fcntl(pfd, F_DUPFD, 30) : -1;
    TEST("pidfd_open + F_DUPFD");
    EXPECT_TRUE(pfd >= 0 && alias >= 30, "setup failed");

    fflush(stdout);
    pid_t checker = fork();
    if (checker == 0) {
        passes = fails = 0;
        TEST("child: pidfd inherited");
        EXPECT_TRUE(fcntl(pfd, F_GETFD) >= 0, "pidfd closed in child");
        TEST("child: alias inherited");
        EXPECT_TRUE(fcntl(alias, F_GETFD) >= 0, "alias closed in child");
        TEST("child: send_signal(0)");
        EXPECT_TRUE(pidfd_send_signal(pfd, 0) == 0, "signal 0 failed");
        TEST("child: not ready while alive");
        EXPECT_TRUE(poll_in(pfd, 0) == 0, "readable while target lives");
        TEST("child: waitid non-child");
        EXPECT_TRUE(waitid_echild(pfd), "waitid did not report ECHILD");

        /* The alias outlives the original only if both name one entry. */
        close(pfd);
        TEST("child: alias after close");
        EXPECT_TRUE(poll_in(alias, 0) == 0 && pidfd_send_signal(alias, 0) == 0,
                    "alias lost its target");
        TEST("child: send_signal(SIGTERM)");
        EXPECT_TRUE(pidfd_send_signal(alias, SIGTERM) == 0, "SIGTERM failed");
        TEST("child: POLLIN on target exit");
        EXPECT_TRUE(poll_in(alias, 10000) == 1, "no exit notification");
        fflush(stdout);
        _exit(fails);
    }

    int checked = checker > 0 && reap_ok(checker);
    TEST("parent: child checks");
    EXPECT_TRUE(checked, "child checks failed");

    /* A child that never reached the target must not leave it running. */
    if (!checked)
        kill(target, SIGKILL);
    int status = 0;
    TEST("parent: target got SIGTERM");
    EXPECT_TRUE(waitpid(target, &status, 0) == target && WIFSIGNALED(status) &&
                    WTERMSIG(status) == SIGTERM,
                "target not killed via inherited pidfd");
    TEST("parent: own pidfd ready");
    EXPECT_TRUE(poll_in(pfd, 10000) == 1, "parent pidfd not ready");
    close(gate[1]);
    close(pfd);
    close(alias);
}

/* Target already exited before the fork. */
static void test_exited_target(void)
{
    pid_t target = fork();
    if (target == 0)
        _exit(3);
    int pfd = pidfd_open(target);
    TEST("pidfd_open(exiting)");
    EXPECT_TRUE(pfd >= 0 && poll_in(pfd, 10000) == 1, "exit not reported");

    fflush(stdout);
    pid_t checker = fork();
    if (checker == 0) {
        passes = fails = 0;
        TEST("child: ready at once");
        EXPECT_TRUE(poll_in(pfd, 0) == 1, "inherited pidfd not ready");
        fflush(stdout);
        _exit(fails);
    }
    TEST("parent: child checks");
    EXPECT_TRUE(checker > 0 && reap_ok(checker), "child checks failed");

    siginfo_t si;
    memset(&si, 0, sizeof(si));
    TEST("parent: waitid(P_PIDFD)");
    EXPECT_TRUE(waitid(P_PIDFD, (id_t) pfd, &si, WEXITED) == 0 &&
                    si.si_pid == target && si.si_status == 3,
                "parent could not reap through its pidfd");
    close(pfd);
}

/* Target is the forking process: the child signals it, and sees it exit. */
static void test_parent_target(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    int self = pidfd_open(getpid());
    TEST("pidfd_open(self)");
    EXPECT_TRUE(self >= 0, "pidfd_open(self) failed");

    fflush(stdout);
    pid_t checker = fork();
    if (checker == 0) {
        passes = fails = 0;
        TEST("child: signal parent");
        EXPECT_TRUE(pidfd_send_signal(self, SIGUSR1) == 0, "SIGUSR1 failed");
        TEST("child: parent alive");
        EXPECT_TRUE(poll_in(self, 0) == 0, "readable while parent lives");
        fflush(stdout);
        _exit(fails);
    }
    TEST("parent: child checks");
    EXPECT_TRUE(checker > 0 && reap_ok(checker), "child checks failed");
    for (int i = 0; i < 500 && !got_usr1; i++)
        usleep(10000);
    TEST("parent: SIGUSR1 delivered");
    EXPECT_TRUE(got_usr1, "signal via inherited self pidfd lost");
    close(self);

    /* A middle process forks a grandchild holding a pidfd on the middle, then
     * exits; the grandchild must see that exit.
     */
    int ready[2], result[2];
    if (pipe(ready) < 0 || pipe(result) < 0) {
        perror("pipe");
        fails++;
        return;
    }
    fflush(stdout);
    pid_t mid = fork();
    if (mid == 0) {
        int mfd = pidfd_open(getpid());
        pid_t gc = fork();
        if (gc == 0) {
            char ok = mfd >= 0 && poll_in(mfd, 0) == 0;
            (void) write(ready[1], "r", 1);
            if (ok)
                ok = poll_in(mfd, 10000) == 1;
            (void) write(result[1], &ok, 1);
            _exit(0);
        }

        /* Only the grandchild may hold the write end, so a failed fork reads
         * EOF instead of waiting forever.
         */
        close(ready[1]);
        char c;
        (void) read(ready[0], &c, 1);
        _exit(0);
    }
    close(ready[0]);
    close(ready[1]);
    close(result[1]);
    TEST("parent: middle exits");
    EXPECT_TRUE(mid > 0 && reap_ok(mid), "middle process failed");
    char ok = 0;
    struct pollfd pf = {.fd = result[0], .events = POLLIN};
    if (poll(&pf, 1, 15000) == 1)
        (void) read(result[0], &ok, 1);
    TEST("grandchild: sees parent exit");
    EXPECT_TRUE(ok == 1, "inherited self pidfd never became ready");
    close(result[0]);
}

int main(void)
{
    printf("test-pidfd-fork: pidfds inherited across fork\n");
    test_sibling_target();
    test_exited_target();
    test_parent_target();
    SUMMARY("test-pidfd-fork");
    return fails ? 1 : 0;
}

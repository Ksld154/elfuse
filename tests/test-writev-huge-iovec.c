/*
 * A huge aliased iovec costs the host nothing it does not have to write
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * writev and pwritev offer every vector to the /proc intercept before the host
 * transfer. That offer used to gather the whole iovec into one host buffer
 * first, so 1024 entries all naming the same 1 GiB mapping asked the host for a
 * 1 TiB allocation and a 1 TiB copy before anyone looked at what the fd was.
 * Here the guest does exactly that against /dev/null and against
 * /proc/self/oom_score_adj, and each call has to come back within the alarm.
 *
 * The /dev/null answer is not asserted to the byte: Linux caps a vectored write
 * at MAX_RW_COUNT and reports that, and the host decides its own cap. What is
 * asserted is that the call returns, without ENOMEM. The oom_score_adj answer
 * is EINVAL on Linux, since the leading bytes it parses are NUL.
 *
 * A small vector to the same node still has to land, and lands the Linux way: a
 * /proc node has a plain write op, so the kernel writes each segment on its
 * own, and the value left behind is the last segment's, not the concatenation.
 *
 * Syscalls exercised: writev(66), pwritev(70), mmap(222), openat(56),
 *                     read(63), setitimer(103)
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define GIB (1ul << 30)
#define NVEC 1024

static struct iovec iov[NVEC];

static void on_alarm(int sig)
{
    (void) sig;
    static const char msg[] = "FAIL: vectored write did not return\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static void arm(int sec)
{
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = sec;
    setitimer(ITIMER_REAL, &it, NULL);
}

static long read_oom_score_adj(void)
{
    char buf[32] = {0};
    int fd = open("/proc/self/oom_score_adj", O_RDONLY);
    if (fd < 0)
        return -99999;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -99999;
    return strtol(buf, NULL, 10);
}

int main(void)
{
    signal(SIGALRM, on_alarm);

    char *big = mmap(NULL, GIB, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (big == MAP_FAILED) {
        FAIL("mmap 1 GiB failed");
        SUMMARY("test-writev-huge-iovec");
        return 1;
    }
    for (int i = 0; i < NVEC; i++) {
        iov[i].iov_base = big;
        iov[i].iov_len = GIB;
    }

    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd < 0) {
        FAIL("open /dev/null failed");
        SUMMARY("test-writev-huge-iovec");
        return 1;
    }
    TEST("writev 1 TiB aliased to /dev/null");
    arm(20);
    ssize_t n = writev(null_fd, iov, NVEC);
    int err = errno;
    arm(0);
    EXPECT_TRUE(n > 0 || (n < 0 && err != ENOMEM), "writev failed with ENOMEM");

    TEST("pwritev 1 TiB aliased to /dev/null");
    arm(20);
    n = pwritev(null_fd, iov, NVEC, 0);
    err = errno;
    arm(0);
    EXPECT_TRUE(n > 0 || (n < 0 && err != ENOMEM),
                "pwritev failed with ENOMEM");
    close(null_fd);

    int oom_fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (oom_fd < 0) {
        FAIL("open oom_score_adj failed");
        SUMMARY("test-writev-huge-iovec");
        return 1;
    }

    TEST("writev 1 TiB of NULs to oom_score_adj");
    arm(20);
    EXPECT_ERRNO(writev(oom_fd, iov, NVEC), EINVAL, "expected EINVAL");
    arm(0);

    TEST("writev segments to oom_score_adj");
    char a[] = "1\n", b[] = "50\n";
    struct iovec small[2] = {{a, 2}, {b, 3}};
    n = writev(oom_fd, small, 2);
    EXPECT_TRUE(n == 5 && read_oom_score_adj() == 50,
                "per-segment writev did not leave 50");

    TEST("pwritev segments to oom_score_adj");
    char c[] = "2", d[] = "5\n";
    struct iovec small2[2] = {{c, 1}, {d, 2}};
    n = pwritev(oom_fd, small2, 2, 0);
    EXPECT_TRUE(n == 3 && read_oom_score_adj() == 5,
                "per-segment pwritev did not leave 5");

    /* Linux copies at most PROC_NUMBUF - 1 (12) bytes and reports that many,
     * which is a short write that stops the vector before the second segment.
     */
    TEST("long segment to oom_score_adj is a 12-byte short write");
    char e[] = "7                   ", f[] = "9\n";
    struct iovec small3[2] = {{e, sizeof(e) - 1}, {f, 2}};
    n = writev(oom_fd, small3, 2);
    EXPECT_TRUE(n == 12 && read_oom_score_adj() == 7,
                "long segment was not cut to the Linux prefix");

    struct iovec reset = {"0\n", 2};
    writev(oom_fd, &reset, 1);
    close(oom_fd);
    munmap(big, GIB);

    SUMMARY("test-writev-huge-iovec");
    return fails ? 1 : 0;
}

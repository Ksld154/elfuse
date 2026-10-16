/*
 * Accept-queue depth of listen
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux lets backlog + 1 connections wait on a listener (sk_acceptq_is_full in
 * include/net/sock.h, unix_recvq_full in net/unix/af_unix.c); macOS lets
 * exactly backlog wait. Each case connects without accepting and counts the
 * connections that complete before one is dropped or refused.
 *
 * macOS TCP sometimes completes one or two connections past its queue (1 run in
 * 30 with plain host sockets), so the TCP cases check only that backlog + 1
 * fit. TCP backlog 0 is left out: elfuse queues 2 there, because host backlog 1
 * loses the urgent mark (src/syscall/net.c, sys_listen).
 *
 * Passes on real Linux.
 *
 * Syscalls exercised: socket(198), bind(200), listen(201), connect(203),
 *                     getsockname(204), ppoll(73), getsockopt(209), close(57),
 *                     mkdirat(34), unlinkat(35)
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

/* A dropped TCP SYN shows as a connect that never completes, so this bounds the
 * wait for the first connection past the queue.
 */
#define CONNECT_WAIT_MS 500

#define MAX_CONNS 16

static char sock_dir[] = "/tmp/elfuse-listen-backlog-XXXXXX";

/* Return the number of connections that complete against listen(@backlog) with
 * none accepted, trying at most @max; -1 when the listener cannot be set up.
 */
static int queue_depth(int family, int backlog, int max)
{
    struct sockaddr_storage ss;
    socklen_t len;
    int cli[MAX_CONNS];
    int opened = 0, done = 0;

    memset(&ss, 0, sizeof(ss));
    if (family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *) &ss;
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof(*in);
    } else {
        struct sockaddr_un *un = (struct sockaddr_un *) &ss;
        un->sun_family = AF_UNIX;
        snprintf(un->sun_path, sizeof(un->sun_path), "%s/s", sock_dir);
        len = sizeof(*un);
    }

    int lst = socket(family, SOCK_STREAM, 0);
    if (lst < 0)
        return -1;
    if (bind(lst, (struct sockaddr *) &ss, len) < 0 ||
        listen(lst, backlog) < 0 ||
        getsockname(lst, (struct sockaddr *) &ss, &len) < 0) {
        close(lst);
        return -1;
    }

    if (max > MAX_CONNS)
        max = MAX_CONNS;
    while (opened < max) {
        int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0)
            break;
        cli[opened++] = fd;
        if (connect(fd, (struct sockaddr *) &ss, len) < 0) {
            if (errno != EINPROGRESS)
                break;
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            int err = 0;
            socklen_t elen = sizeof(err);
            if (poll(&p, 1, CONNECT_WAIT_MS) != 1 ||
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err)
                break;
        }
        done++;
    }

    for (int i = 0; i < opened; i++)
        close(cli[i]);
    close(lst);
    if (family == AF_UNIX)
        unlink(((struct sockaddr_un *) &ss)->sun_path);
    return done;
}

static void expect_depth(const char *name, int family, int backlog)
{
    TEST(name);
    int depth = queue_depth(family, backlog, backlog + 3);
    bool ok = family == AF_INET ? depth >= backlog + 1 : depth == backlog + 1;
    if (ok) {
        PASS();
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "backlog %d queued %d, want %s%d", backlog,
             depth, family == AF_INET ? ">= " : "", backlog + 1);
    FAIL(msg);
}

int main(void)
{
    printf("test-listen-backlog\n");

    if (!mkdtemp(sock_dir)) {
        FAIL("mkdtemp failed");
        SUMMARY("test-listen-backlog");
        return 1;
    }

    expect_depth("TCP backlog 1 queues >= 2", AF_INET, 1);
    expect_depth("TCP backlog 4 queues >= 5", AF_INET, 4);
    expect_depth("AF_UNIX backlog 0 queues 1", AF_UNIX, 0);
    expect_depth("AF_UNIX backlog 1 queues 2", AF_UNIX, 1);
    expect_depth("AF_UNIX backlog 4 queues 5", AF_UNIX, 4);

    rmdir(sock_dir);
    SUMMARY("test-listen-backlog");
    return fails ? 1 : 0;
}

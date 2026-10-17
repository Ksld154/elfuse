/*
 * pidfd syscall wrappers for tests
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The cross toolchain's libc predates pidfd_open and pidfd_send_signal, so the
 * tests reach them through syscall(2), which reports failures through errno the
 * way the libc wrappers would.
 */

#pragma once

#include <sys/syscall.h>
#include <unistd.h>

#define NR_pidfd_open 434
#define NR_pidfd_send_signal 424
#ifndef P_PIDFD
#define P_PIDFD 3
#endif

static inline int pidfd_open(pid_t pid)
{
    return (int) syscall(NR_pidfd_open, pid, 0);
}

static inline int pidfd_send_signal(int fd, int sig)
{
    return (int) syscall(NR_pidfd_send_signal, fd, sig, NULL, 0);
}

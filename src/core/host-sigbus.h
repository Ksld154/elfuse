/*
 * Host SIGBUS recovery around direct guest memory access
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Kept in core/ so the guest memory helpers in core/guest.c can arm recovery
 * without reaching into the syscall layer. The handler and the per-thread
 * recovery slot live in syscall/signal.c.
 */

#pragma once

#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>

typedef struct {
    jmp_buf env;
    volatile sig_atomic_t armed;
} host_sigbus_recovery_t;

/* Arm per-thread host SIGBUS recovery for direct guest memory copies. */
host_sigbus_recovery_t *signal_host_sigbus_recovery(void);

/* Run the trailing statement with host SIGBUS recovery armed for this thread,
 * setting faulted to true when it took a fault instead of killing the host.
 *
 * A MAP_SHARED overlay stays live in the guest slab after anything truncates
 * the file, so a host access to the vanished page raises SIGBUS. Only host user
 * mode needs this; kernel copyin/copyout reports EFAULT instead. The handler
 * returns normally and the kernel restores the interrupted mask before the
 * resume point runs, which is what lets the mask-free _setjmp serve here.
 *
 * Rules for the statement, worst breakage first:
 * - No return, break, goto, or longjmp out. That skips the disarm and leaves
 *   env naming a dead frame, so the next fault jumps into freed stack.
 * - No lock, including one inside a callee: arc4random_buf here stranded a libc
 *   lock and hung the next fork. Keep to memcpy, memchr, atomic builtins.
 * - No result in a non-volatile local read when faulted; _longjmp leaves those
 *   indeterminate.
 * - No nesting: an inner guard disarms the outer on exit.
 */
#define HOST_SIGBUS_GUARD(faulted, ...)                               \
    do {                                                              \
        host_sigbus_recovery_t *hsr_ = signal_host_sigbus_recovery(); \
        if (_setjmp(hsr_->env) != 0) {                                \
            signal_host_sigbus_recovery()->armed = 0;                 \
            (faulted) = true;                                         \
        } else {                                                      \
            hsr_->armed = 1;                                          \
            __VA_ARGS__;                                              \
            hsr_->armed = 0;                                          \
            (faulted) = false;                                        \
        }                                                             \
    } while (0)

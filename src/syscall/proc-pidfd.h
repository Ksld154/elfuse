/*
 * pidfd helpers
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/guest.h"

void pidfd_init(void);

/* Create a pidfd on @target_pid. @host_pid is the host process to watch for its
 * exit, or <= 0 when the target lives in this host process and so cannot be
 * watched (the caller itself, or a CLONE_VM child).
 */
int pidfd_create(guest_t *g, int64_t target_pid, pid_t host_pid);

/* Hold the registry entry for the description ofd_id across a dup, so a sibling
 * closing the source before the alias is published cannot retire it. pidfd_pin
 * returns false when no entry names ofd_id any more; every true return is
 * balanced by pidfd_unpin once the alias is published or abandoned. Neither may
 * be called with fd_lock held.
 */
bool pidfd_pin(uint64_t ofd_id);
void pidfd_unpin(uint64_t ofd_id);

/* What a fork child needs to rebuild one inherited pidfd description. */
typedef struct {
    int64_t target_pid; /* -1 when no registry entry names the description */
    int32_t host_pid;   /* the host process to watch, 0 for none */
    /* Nonzero once the write end is closed: the exit was reported, or the
     * monitor could not be armed, so the target may still be alive.
     */
    int32_t exited;
} pidfd_fork_state_t;

/* Caller holds fd_lock, which keeps the description named by ofd_id live. */
void pidfd_fork_snapshot_locked(uint64_t ofd_id, pidfd_fork_state_t *out);

/* Fork child: rebuild the pidfd at guest_fd with its own pipe, registry entry
 * and monitor. Further aliases of the same description dup the slot this
 * returns, so they share the entry.
 *
 * Returns guest_fd or a negative Linux errno.
 */
int pidfd_fork_restore(int guest_fd, const pidfd_fork_state_t *state);

void proc_pidfd_notify_exit(int64_t exited_pid);
int64_t proc_pidfd_lookup_pid(int guest_fd);
int64_t sys_pidfd_open(guest_t *g, int64_t pid, unsigned int flags);
int64_t sys_pidfd_send_signal(guest_t *g,
                              int pidfd,
                              int sig,
                              uint64_t info_gva,
                              unsigned int flags);

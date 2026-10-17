/*
 * iovec accumulation arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * readv, writev, preadv, pwritev, recvmsg and sendmsg all take an iovec array
 * the guest wrote, and every one of them sums iov_len across it. Linux returns
 * EINVAL when that sum exceeds SSIZE_MAX; elfuse has to do the same, because
 * the sum decides the transfer length and the completion test of every one of
 * those paths.
 *
 * io.c carried four copies of the accumulation with three different guards. One
 * proved add now replaces all four (urandom_fill_iov, validate_iov_total,
 * proc_try_writev_intercept and process_vm_import_iov), and the readv/writev
 * loop in io.c, net_send_xfer and the netlink receive call it too.
 *
 * Split into a header because io.c and net-msg.c cannot be given to Frama-C:
 * they include the macOS uio and socket headers, which the analyzer's libc does
 * not model. This header needs only stdint.h and sys/uio.h, and the analyzer
 * supplies its own modeled sys/uio.h, so make verify-iov proves it directly.
 */

#pragma once

#include <stdint.h>
#include <sys/uio.h>

/* Linux UIO_MAXIOV: the cap on iovcnt every one of these syscalls enforces. */
#define IOV_COUNT_MAX 1024LL

/* SSIZE_MAX on LP64, spelled out rather than included: this is the guest's
 * ssize_t, and the value Linux compares the running total against.
 */
#define IOV_TOTAL_MAX 0x7FFFFFFFFFFFFFFFULL

_Static_assert(IOV_TOTAL_MAX == (uint64_t) INT64_MAX,
               "the total cap is the guest's SSIZE_MAX");

/* Whether an iovec count is one Linux would accept.
 *
 * Takes int64_t rather than int so a caller holding a wider count can pass it
 * without narrowing first. Only the readv family uses it today, and with an
 * int. sendmsg and recvmsg keep their own msg_iovlen cap and have to: they
 * accept msg_iovlen == 0, which this check rejects, so it cannot serve them.
 */
/*@
  assigns \nothing;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (1 <= iovcnt <= IOV_COUNT_MAX);
 */
static inline int iov_count_ok(int64_t iovcnt)
{
    return iovcnt >= 1 && iovcnt <= IOV_COUNT_MAX;
}

/* Add one entry's length to a running total, or 0 when that would carry the
 * total past SSIZE_MAX.
 *
 * The guard is written as "len > IOV_TOTAL_MAX - total" rather than "total +
 * len > IOV_TOTAL_MAX" for the obvious reason: the second form has already
 * overflowed by the time it is tested. That the two are not equivalent is
 * exactly what a reviewer skims past, so it is stated as a postcondition
 * instead.
 */
/*@
  requires total <= IOV_TOTAL_MAX;
  requires \valid(out);
  assigns *out;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> len <= IOV_TOTAL_MAX - total;
  ensures \result != 0 ==> *out == total + len;
  ensures \result != 0 ==> *out <= IOV_TOTAL_MAX;
  ensures \result != 0 ==> *out >= total;
  ensures \result == 0 ==> *out == \old(*out);
 */
static inline int iov_total_add(uint64_t total, uint64_t len, uint64_t *out)
{
    if (len > IOV_TOTAL_MAX - total)
        return 0;

    *out = total + len;
    return 1;
}

/* How many iovec entries a partial transfer of `moved` bytes has fully spent,
 * and how far into the first survivor it landed.
 *
 * The caller resumes at iov + result with iovcnt - result entries, after
 * trimming that survivor by *rem_out. Splitting the index arithmetic out from
 * the pointer bump is what makes the useful half provable: the two facts a
 * caller needs before touching the survivor are that the index is in range and
 * that the remainder is strictly inside it, and both are postconditions here.
 * The bump itself stays in io.c, since iov_base points into guest memory whose
 * extent no contract in this tree can name.
 *
 * The last postcondition is what ties the remainder back to the bytes moved.
 * Without it the contract is satisfied by a loop that never subtracts anything,
 * since the exit condition alone already puts rem below the entry it indexes;
 * make verify-mutants found exactly that hole by deleting the subtraction.
 *
 * The separation precondition is not ceremony: without it *rem_out and the
 * array may alias, the store can change the length the second postcondition
 * talks about, and the proof fails. Every caller passes a local.
 */
/*@
  requires 0 <= iovcnt;
  requires \valid_read(iov + (0 .. iovcnt - 1));
  requires \valid(rem_out);
  requires \separated(rem_out, iov + (0 .. iovcnt - 1));
  assigns *rem_out;
  ensures 0 <= \result <= iovcnt;
  ensures \result < iovcnt ==> *rem_out < iov[\result].iov_len;
  ensures *rem_out <= moved;
  ensures \result > 0 ==> *rem_out + iov[\result - 1].iov_len <= moved;
 */
static inline int iov_advance_index(const struct iovec *iov,
                                    int iovcnt,
                                    size_t moved,
                                    size_t *rem_out)
{
    int spent = 0;
    size_t rem = moved;

    /*@
      loop invariant 0 <= spent <= iovcnt;
      loop invariant rem <= moved;
      loop invariant spent > 0 ==> rem + iov[spent - 1].iov_len <= moved;
      loop assigns spent, rem;
      loop variant iovcnt - spent;
     */
    while (spent < iovcnt && rem >= iov[spent].iov_len) {
        rem -= iov[spent].iov_len;
        spent++;
    }
    *rem_out = rem;
    return spent;
}

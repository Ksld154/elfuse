/*
 * FUSE wire ABI for the in-guest daemon tests
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The opcodes and the request and reply layouts of include/uapi/linux/fuse.h
 * that the test daemons speak, spelled once for every test that serves
 * /dev/fuse itself. Only what the tests use; extend it rather than redeclaring
 * a struct in a test.
 */

#pragma once

#include <stdint.h>

#ifndef SYS_mount
#define SYS_mount 40
#endif

#define FUSE_ROOT_ID 1
#define FUSE_ASYNC_READ (1u << 0)
#define FUSE_BIG_WRITES (1u << 5)
#define FUSE_MAX_PAGES (1u << 22)

enum fuse_opcode {
    FUSE_LOOKUP = 1,
    FUSE_FORGET = 2,
    FUSE_GETATTR = 3,
    FUSE_OPEN = 14,
    FUSE_READ = 15,
    FUSE_RELEASE = 18,
    FUSE_INIT = 26,
    FUSE_OPENDIR = 27,
    FUSE_READDIR = 28,
    FUSE_RELEASEDIR = 29,
    FUSE_ACCESS = 34,
    FUSE_INTERRUPT = 36,
    FUSE_BATCH_FORGET = 42,
};

struct fuse_attr {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec;
    uint32_t mode, nlink, uid, gid, rdev, blksize, flags;
};

struct fuse_entry_out {
    uint64_t nodeid, generation, entry_valid, attr_valid;
    uint32_t entry_valid_nsec, attr_valid_nsec;
    struct fuse_attr attr;
};

struct fuse_attr_out {
    uint64_t attr_valid;
    uint32_t attr_valid_nsec;
    uint32_t dummy;
    struct fuse_attr attr;
};

struct fuse_open_out {
    uint64_t fh;
    uint32_t open_flags;
    int32_t backing_id;
};

struct fuse_read_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t read_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

struct fuse_forget_in {
    uint64_t nlookup;
};

struct fuse_interrupt_in {
    uint64_t unique;
};

struct fuse_batch_forget_in {
    uint32_t count;
    uint32_t dummy;
};

struct fuse_forget_one {
    uint64_t nodeid;
    uint64_t nlookup;
};

struct fuse_init_out {
    uint32_t major, minor, max_readahead, flags;
    uint16_t max_background, congestion_threshold;
    uint32_t max_write, time_gran;
    uint16_t max_pages, map_alignment;
    uint32_t flags2, max_stack_depth;
    uint16_t request_timeout, unused[11];
};

struct fuse_in_header {
    uint32_t len, opcode;
    uint64_t unique, nodeid;
    uint32_t uid, gid, pid;
    uint16_t total_extlen, padding;
};

struct fuse_out_header {
    uint32_t len;
    int32_t error;
    uint64_t unique;
};

struct fuse_dirent {
    uint64_t ino;
    uint64_t off;
    uint32_t namelen;
    uint32_t type;
    char name[];
};

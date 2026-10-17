/*
 * Symlink chains spliced through the openat2 RESOLVE_NO_XDEV walker
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * From the second link on, the walker splices each link target into the very
 * buffer the unconsumed remainder of the path still lives in. When the new
 * target is longer than the consumed prefix, a splice that writes before it
 * reads overwrites the remainder, and the walk goes on with a different path
 * than the one the guest asked for. The remainder here ends in a link to /proc,
 * so a corrupted walk loses the mount crossing and openat2 stops reporting
 * EXDEV.
 *
 * Every chain grows its link names at each hop, so each spliced target is
 * longer than what the walk has consumed of the previous one.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "linux-openat2.h"
#include "test-harness.h"

int passes = 0, fails = 0;

#define CHAIN_MAX 5
#define TARGET_DIR "target-directory-with-a-long-name"

static long openat2_noxdev(int dirfd, const char *path)
{
    struct open_how how = {
        .flags = O_RDONLY, .mode = 0, .resolve = RESOLVE_NO_XDEV};

    return syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
}

/* Name hop @hop of chain @chain: a letter per chain, repeated so each hop is
 * longer than the one before it.
 */
static void link_name(char *buf, size_t bufsz, int chain, int hop)
{
    size_t len = (size_t) (4 * (hop + 1) + chain);

    if (len >= bufsz)
        len = bufsz - 1;
    memset(buf, 'a' + chain, len);
    buf[len] = '\0';
}

static int make_chain(int dirfd, int chain, int links)
{
    char name[64], next[64];

    for (int hop = 0; hop < links; hop++) {
        link_name(name, sizeof(name), chain, hop);
        if (hop + 1 < links)
            link_name(next, sizeof(next), chain, hop + 1);
        else
            snprintf(next, sizeof(next), "%s", TARGET_DIR);
        if (symlinkat(next, dirfd, name) < 0)
            return -1;
    }
    return 0;
}

/* Sysroot mode: @dir holds links l2..l5, each the head of a chain of that many
 * relative links ending at realdir, where realdir/abs is an absolute link to a
 * host directory outside the sysroot holding secret.txt. The sysroot resolver
 * refuses a relative link that lands on the host with ELOOP, and the descriptor
 * walk that then decides whether the chain reaches an absolute bridge splices
 * every hop into one buffer, as the openat2 walker does. The bridge sits in the
 * remainder, so a splice that clobbers the remainder never finds it.
 */
static int run_sysroot_chains(const char *dir)
{
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        FAIL("open staged dir");
        SUMMARY("test-symlink-chain-splice");
        return 1;
    }

    for (int links = 2; links <= CHAIN_MAX; links++) {
        char path[64], label[64], buf[8] = {0};

        snprintf(label, sizeof(label), "%d-link sysroot chain opens", links);
        TEST(label);
        snprintf(path, sizeof(path), "l%d/abs/secret.txt", links);
        int fd = openat(dirfd, path, O_RDONLY);
        if (fd < 0)
            FAIL("openat failed");
        else if (read(fd, buf, sizeof(buf)) != 2 || strcmp(buf, "ok"))
            FAIL("wrong contents");
        else
            PASS();
        if (fd >= 0)
            close(fd);
    }
    close(dirfd);

    SUMMARY("test-symlink-chain-splice");
    return fails > 0;
}

int main(int argc, char **argv)
{
    printf("test-symlink-chain-splice: symlink chains through openat2\n");

    if (argc > 1)
        return run_sysroot_chains(argv[1]);

    char dir[] = "/tmp/elfuse-chain-splice-XXXXXX";
    if (!mkdtemp(dir)) {
        FAIL("mkdtemp");
        SUMMARY("test-symlink-chain-splice");
        return 1;
    }
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0 || mkdirat(dirfd, TARGET_DIR, 0755) < 0) {
        FAIL("setup target dir");
        SUMMARY("test-symlink-chain-splice");
        return 1;
    }

    int fd = openat(dirfd, TARGET_DIR "/f", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "ok", 2) != 2 ||
        symlinkat("/proc/self", dirfd, TARGET_DIR "/p") < 0) {
        FAIL("setup target contents");
        SUMMARY("test-symlink-chain-splice");
        return 1;
    }
    close(fd);

    for (int links = 2; links <= CHAIN_MAX; links++) {
        int chain = links - 2;
        char head[64], path[128], label[96];

        if (make_chain(dirfd, chain, links) < 0) {
            FAIL("symlinkat chain");
            continue;
        }
        link_name(head, sizeof(head), chain, 0);

        snprintf(label, sizeof(label), "%d-link chain opens file", links);
        TEST(label);
        snprintf(path, sizeof(path), "%s/f", head);
        long rfd = openat2_noxdev(dirfd, path);
        char buf[8] = {0};
        if (rfd < 0)
            FAIL("openat2 failed");
        else if (read((int) rfd, buf, sizeof(buf)) != 2 || strcmp(buf, "ok"))
            FAIL("wrong contents");
        else
            PASS();
        if (rfd >= 0)
            close((int) rfd);

        snprintf(label, sizeof(label), "%d-link chain sees /proc cross", links);
        TEST(label);
        snprintf(path, sizeof(path), "%s/p/status", head);
        rfd = openat2_noxdev(dirfd, path);
        if (rfd >= 0) {
            close((int) rfd);
            FAIL("expected EXDEV, open succeeded");
        } else if (errno != EXDEV) {
            FAIL("expected EXDEV");
        } else {
            PASS();
        }

        for (int hop = 0; hop < links; hop++) {
            link_name(head, sizeof(head), chain, hop);
            unlinkat(dirfd, head, 0);
        }
    }

    unlinkat(dirfd, TARGET_DIR "/p", 0);
    unlinkat(dirfd, TARGET_DIR "/f", 0);
    unlinkat(dirfd, TARGET_DIR, AT_REMOVEDIR);
    close(dirfd);
    rmdir(dir);

    SUMMARY("test-symlink-chain-splice");
    return fails > 0;
}

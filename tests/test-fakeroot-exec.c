/*
 * ELFUSE_FAKEROOT_EXEC opt-in privilege transition
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fakeroot can otherwise only be armed before the first guest image runs, so a
 * guest shell has no way to raise privilege for one command the way sudo does
 * on Linux. ELFUSE_FAKEROOT_EXEC names a single executable whose exec enters
 * fakeroot: exec it and the new image runs as root; exec anything else and the
 * caller stays unprivileged.
 *
 * The test re-execs itself, so the matrix points ELFUSE_FAKEROOT_EXEC at this
 * binary. It checks the initial image is unprivileged, that the marked exec
 * lands at uid/gid 0, that root survives the fork into a fresh host process,
 * that another spelling of the same file elevates too (the match is on file
 * identity, not on the pathname), that execveat reaches the same decision, and
 * that a copy of the same program at a different path does not elevate.
 *
 * Not covered here: a sysroot lane, where the guest and host spellings of the
 * marked file genuinely differ; a marked path that is a shebang script; and
 * what a sibling *thread* observes when another thread execs the marked binary,
 * which needs a same-process test rather than the fork-based ones below.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-harness.h"

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

int passes = 0, fails = 0;

extern char **environ;

/* Child modes report through the exit status: 0 when the observed credentials
 * match what the mode expects, 1 otherwise. The parent turns that into a check.
 */
#define CHILD_OK 0
#define CHILD_BAD 1

static int all_ids_are(uid_t want)
{
    return getuid() == want && geteuid() == want && getgid() == want &&
           getegid() == want;
}

/* True when none of the four ids is root. The group ids matter as much as the
 * user ids here: the contract for an unmarked exec is that credentials are
 * untouched, not merely that uid stayed non-zero.
 */
static int no_id_is_root(void)
{
    return getuid() != 0 && geteuid() != 0 && getgid() != 0 && getegid() != 0;
}

static void report_ids(const char *what)
{
    fprintf(stderr, "%s: uid=%d euid=%d gid=%d egid=%d\n", what, (int) getuid(),
            (int) geteuid(), (int) getgid(), (int) getegid());
}

/* Exec'd as the ELFUSE_FAKEROOT_EXEC path: must be root, and a fork from here
 * must land in a host process that is still root.
 */
static int mode_expect_root(void)
{
    if (!all_ids_are(0)) {
        report_ids("child: expected root");
        return CHILD_BAD;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("child: fork");
        return CHILD_BAD;
    }
    if (pid == 0)
        _exit(all_ids_are(0) ? CHILD_OK : CHILD_BAD);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("child: waitpid");
        return CHILD_BAD;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != CHILD_OK) {
        fprintf(stderr, "child: forked grandchild lost root (status=%d)\n",
                status);
        return CHILD_BAD;
    }
    return CHILD_OK;
}

/* Exec'd under any other path: credentials must be untouched. */
static int mode_expect_unprivileged(void)
{
    if (!no_id_is_root()) {
        report_ids("child: unexpected root");
        return CHILD_BAD;
    }
    return CHILD_OK;
}

/* Exit status a child uses when the exec itself never happened, so a staging
 * mistake cannot be misread as a verdict about credentials.
 */
#define CHILD_NO_EXEC 127

/* Run prog with a single mode argument and return its exit status, or -1.
 * use_execveat picks the execveat(AT_EMPTY_PATH) entry point over execve, which
 * hands sys_execve a host path it resolved itself rather than the guest string.
 */
static int run_child_via(const char *prog, const char *mode, int use_execveat)
{
    char *argv[] = {(char *) prog, (char *) mode, NULL};

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        if (use_execveat) {
            int fd = open(prog, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                perror("child: open for execveat");
                _exit(CHILD_NO_EXEC);
            }
            syscall(SYS_execveat, fd, "", argv, environ, AT_EMPTY_PATH);
            perror("child: execveat");
        } else {
            execve(prog, argv, environ);
            perror("child: execve");
        }
        _exit(CHILD_NO_EXEC);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "child did not exit normally (status=%d)\n", status);
        return -1;
    }
    if (WEXITSTATUS(status) == CHILD_NO_EXEC)
        fprintf(stderr, "child never exec'd %s\n", prog);
    return WEXITSTATUS(status);
}

static int run_child(const char *prog, const char *mode)
{
    return run_child_via(prog, mode, 0);
}

/* Copy the program to a private path so the negative case is a different file
 * rather than a spelling trick on the configured one.
 */
static int copy_program(const char *src, char *dst, size_t dst_sz)
{
    /* mkstemp, not a pid-derived name: it creates the file exclusively, so a
     * pre-created path (or a symlink planted at one) cannot redirect the copy.
     */
    snprintf(dst, dst_sz, "/tmp/elfuse-fakeroot-exec-copy-XXXXXX");

    int in = open(src, O_RDONLY);
    if (in < 0) {
        perror("copy: open src");
        return -1;
    }
    int out = mkstemp(dst);
    if (out < 0) {
        perror("copy: mkstemp");
        close(in);
        return -1;
    }

    /* mkstemp creates 0600. The copy needs the other-execute bit: its host
     * owner is the real macOS uid, which never matches the emulated guest uid,
     * so exec_check_permission only ever consults the other bits.
     */
    int rc = 0;
    if (fchmod(out, 0755) < 0) {
        perror("copy: fchmod");
        rc = -1;
    }

    char buf[65536];
    ssize_t n = 0;
    while (rc == 0 && (n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t) (n - off));
            if (w < 0) {
                perror("copy: write");
                rc = -1;
                break;
            }
            off += w;
        }
    }
    if (rc == 0 && n < 0) {
        perror("copy: read");
        rc = -1;
    }
    close(in);
    /* Close before exec: a still-open writable fd makes execve fail ETXTBSY. */
    if (close(out) < 0 && rc == 0) {
        perror("copy: close dst");
        rc = -1;
    }

    /* One drop site for the staged file: mkstemp already created it, so every
     * failure from here on has to leave nothing behind in the shared /tmp.
     */
    if (rc < 0)
        unlink(dst);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "expect-root"))
        return mode_expect_root();
    if (argc > 1 && !strcmp(argv[1], "expect-unprivileged"))
        return mode_expect_unprivileged();

    const char *marked = getenv("ELFUSE_FAKEROOT_EXEC");
    if (!marked || !*marked) {
        /* A hard failure, not a skip: the matrix always exports this, so an
         * unset variable means the harness stopped arming the feature and the
         * whole file would otherwise report a silent pass.
         */
        printf("test-fakeroot-exec: ELFUSE_FAKEROOT_EXEC is not set\n");
        return 1;
    }

    printf("ELFUSE_FAKEROOT_EXEC tests (marked=%s)\n", marked);

    TEST("initial image unprivileged");
    EXPECT_TRUE(no_id_is_root(), "started as root");

    TEST("exec of marked path is root");
    EXPECT_EQ(run_child(marked, "expect-root"), CHILD_OK,
              "marked exec did not reach root");

    /* Cross-process only: run_child forks, and a guest fork is a separate host
     * process with its own identity globals, so this pins that elevation does
     * not leak back across that boundary. What a surviving sibling *thread*
     * sees is a different question and needs a same-process test.
     */
    TEST("elevation stays in the exec'd process");
    EXPECT_TRUE(no_id_is_root(), "parent gained root");

    /* Identity, not spelling: "<dir>/./<base>" opens the very same file, so it
     * must elevate too. The inverse -- a different file -- is the copy below.
     */
    char spelled[PATH_MAX];
    const char *base = strrchr(marked, '/');
    /* Always non-NULL: elfuse rejects a non-absolute ELFUSE_FAKEROOT_EXEC. */
    int spelled_len = snprintf(spelled, sizeof(spelled), "%.*s/.%s",
                               (int) (base - marked), marked, base);
    TEST("other spelling of marked file is root");
    if (spelled_len < 0 || (size_t) spelled_len >= sizeof(spelled))
        FAIL("respelled path did not fit");
    else
        EXPECT_EQ(run_child(spelled, "expect-root"), CHILD_OK,
                  "same file under another name did not reach root");

    TEST("execveat of marked file is root");
    EXPECT_EQ(run_child_via(marked, "expect-root", 1), CHILD_OK,
              "execveat did not reach root");

    char copy[PATH_MAX];
    if (copy_program(marked, copy, sizeof(copy)) == 0) {
        TEST("exec of another file stays unprivileged");
        EXPECT_EQ(run_child(copy, "expect-unprivileged"), CHILD_OK,
                  "unmarked exec gained root or would not run");
        unlink(copy);
    } else {
        TEST("exec of another file stays unprivileged");
        FAIL("could not stage program copy");
    }

    SUMMARY("test-fakeroot-exec");
    return fails == 0 ? 0 : 1;
}

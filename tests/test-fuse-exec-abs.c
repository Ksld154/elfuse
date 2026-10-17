/*
 * Absolute FUSE paths use guest inode permissions for access and execve.
 *
 * Linux decides a FUSE exec in two ways. With default_permissions the kernel
 * judges the mode as for any inode, so an owner without its own execute bit is
 * refused. Without it the daemon owns access, and the kernel refuses only a
 * mode with no execute bit at all. Either way a node that is not a regular file
 * is refused before anything is read.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linux-fuse.h"

static const char file_name[] = "prog";
static const char child_marker[] = "fuse-exec-abs-child";
static const char denied_marker[] = "fuse-exec-denied";

static uint8_t *image;
static size_t image_len;
static _Atomic unsigned file_mode = 0755;
static _Atomic unsigned file_type = S_IFREG;

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static void load_self(void)
{
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0)
        die("open(/proc/self/exe)");
    struct stat st;
    if (fstat(fd, &st) < 0)
        die("fstat(/proc/self/exe)");
    image_len = (size_t) st.st_size;
    image = malloc(image_len);
    if (!image)
        die("malloc");
    for (size_t off = 0; off < image_len;) {
        ssize_t nr = read(fd, image + off, image_len - off);
        if (nr <= 0)
            die("read(/proc/self/exe)");
        off += (size_t) nr;
    }
    close(fd);
}

static void fill_attr(struct fuse_attr *attr, int dir)
{
    memset(attr, 0, sizeof(*attr));
    attr->ino = dir ? 1 : 2;
    attr->mode = dir ? (S_IFDIR | 0755)
                     : (atomic_load_explicit(&file_type, memory_order_relaxed) |
                        atomic_load_explicit(&file_mode, memory_order_relaxed));
    attr->nlink = dir ? 2 : 1;
    attr->uid = getuid();
    attr->gid = getgid();
    attr->size = dir ? 0 : image_len;
    attr->blocks = dir ? 0 : (image_len + 511) / 512;
    attr->blksize = 4096;
}

static void reply(int fd,
                  uint64_t unique,
                  int32_t error,
                  const void *p,
                  size_t len)
{
    static uint8_t buf[1 << 17];
    struct fuse_out_header out = {
        .len = (uint32_t) (sizeof(out) + len),
        .error = error,
        .unique = unique,
    };
    memcpy(buf, &out, sizeof(out));
    if (len)
        memcpy(buf + sizeof(out), p, len);
    if (write(fd, buf, sizeof(out) + len) != (ssize_t) (sizeof(out) + len))
        die("write(/dev/fuse)");
}

static void *daemon_main(void *arg)
{
    int fusefd = *(int *) arg;
    static uint8_t buf[1 << 17];
    for (;;) {
        ssize_t nr = read(fusefd, buf, sizeof(buf));
        if (nr < 0) {
            if (errno == ENOTCONN || errno == EBADF || errno == EINTR)
                return NULL;
            die("read(/dev/fuse)");
        }
        struct fuse_in_header *in = (struct fuse_in_header *) buf;
        switch (in->opcode) {
        case FUSE_INIT: {
            struct fuse_init_out out = {
                .major = 7,
                .minor = 45,
                .max_readahead = 1024 * 1024,
                .max_write = 65536,
            };
            reply(fusefd, in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_GETATTR: {
            struct fuse_attr_out out;
            memset(&out, 0, sizeof(out));
            fill_attr(&out.attr, in->nodeid == FUSE_ROOT_ID);
            reply(fusefd, in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_LOOKUP: {
            const char *name = (const char *) (in + 1);
            if (in->nodeid != FUSE_ROOT_ID || strcmp(name, file_name)) {
                reply(fusefd, in->unique, -ENOENT, NULL, 0);
                break;
            }
            struct fuse_entry_out out;
            memset(&out, 0, sizeof(out));
            out.nodeid = 2;
            fill_attr(&out.attr, 0);
            reply(fusefd, in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_OPEN: {
            struct fuse_open_out out = {.fh = 11};
            reply(fusefd, in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_READ: {
            struct fuse_read_in *rin = (struct fuse_read_in *) (in + 1);
            size_t avail =
                rin->offset < image_len ? image_len - rin->offset : 0;
            if (avail > rin->size)
                avail = rin->size;
            reply(fusefd, in->unique, 0, image + rin->offset, avail);
            break;
        }
        case FUSE_ACCESS:
        case FUSE_RELEASE:
            reply(fusefd, in->unique, 0, NULL, 0);
            break;
        case FUSE_FORGET:
        case FUSE_BATCH_FORGET:
        case FUSE_INTERRUPT:
            break;
        default:
            reply(fusefd, in->unique, -ENOSYS, NULL, 0);
            break;
        }
    }
}

/* Runs in a forked child so the parent, which never mounts anything, can remove
 * the host directory under the mount once the child is gone: a successful
 * execve never returns to clean up after itself.
 */
static int run_child(const char *mount_dir, int default_perms)
{
    load_self();

    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/%s", mount_dir, file_name);

    int fusefd = open("/dev/fuse", O_RDWR);
    if (fusefd < 0)
        die("open(/dev/fuse)");
    pthread_t daemon;
    int rc = pthread_create(&daemon, NULL, daemon_main, &fusefd);
    if (rc != 0) {
        errno = rc;
        die("pthread_create(daemon)");
    }

    char opts[128];
    snprintf(opts, sizeof(opts),
             "fd=%d,rootmode=40000,user_id=%u,group_id=%u%s", fusefd,
             (unsigned) getuid(), (unsigned) getgid(),
             default_perms ? ",default_permissions" : "");
    if (syscall(SYS_mount, "elfuse-test", mount_dir, "fuse", 0, opts) < 0)
        die("mount(fuse)");

    if (access(file_path, X_OK) < 0) {
        fprintf(stderr, "access(%s) failed: errno=%d\n", file_path, errno);
        return 1;
    }

    char *cargv[] = {(char *) file_name, (char *) denied_marker, NULL};
    char *envp[] = {NULL};
    atomic_store_explicit(&file_mode, 0000, memory_order_relaxed);
    execve(file_path, cargv, envp);
    if (errno != EACCES)
        die("execve(FUSE image without execute permission)");
    atomic_store_explicit(&file_type, S_IFIFO, memory_order_relaxed);
    atomic_store_explicit(&file_mode, 0111, memory_order_relaxed);
    execve(file_path, cargv, envp);
    if (errno != EACCES)
        die("execve(FUSE FIFO)");
    atomic_store_explicit(&file_type, S_IFREG, memory_order_relaxed);

    /* An owner without its own execute bit: refused when the kernel judges the
     * mode, allowed when the daemon owns access. Root passes either way.
     */
    unsigned final_mode = 0111;
    if (geteuid() != 0) {
        atomic_store_explicit(&file_mode, 0011, memory_order_relaxed);
        if (default_perms) {
            execve(file_path, cargv, envp);
            if (errno != EACCES)
                die("execve(FUSE image without owner execute permission)");
        } else {
            final_mode = 0011;
        }
    }
    cargv[1] = (char *) child_marker;
    atomic_store_explicit(&file_mode, final_mode, memory_order_relaxed);
    execve(file_path, cargv, envp);
    fprintf(stderr, "execve(%s) failed: errno=%d\n", file_path, errno);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], denied_marker)) {
        fprintf(stderr, "execve accepted a file without execute permission\n");
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], child_marker)) {
        printf("test-fuse-exec-abs: child ran\n");
        return 0;
    }

    for (int default_perms = 0; default_perms <= 1; default_perms++) {
        char mount_dir[] = "/tmp/elfuse-fuse-exec-abs.XXXXXX";
        if (!mkdtemp(mount_dir))
            die("mkdtemp");

        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0)
            die("fork");
        if (pid == 0)
            _exit(run_child(mount_dir, default_perms));

        int status = 0;
        if (waitpid(pid, &status, 0) != pid)
            die("waitpid");
        rmdir(mount_dir);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "child (default_permissions=%d) status 0x%x\n",
                    default_perms, status);
            return 1;
        }
    }
    printf("test-fuse-exec-abs: ok\n");
    return 0;
}

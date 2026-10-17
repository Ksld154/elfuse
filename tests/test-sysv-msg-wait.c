/*
 * SysV message queue waits: a blocking msgrcv or msgsnd is woken by another
 * process, interrupted by a signal, and does not outlive an exit_group or an
 * execve taken by a sibling thread.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

int passes = 0, fails = 0;

struct small_msg {
    long mtype;
    char mtext[16];
};

static int send_one(int id, long type, const char *text, int flags)
{
    struct small_msg m = {.mtype = type};
    snprintf(m.mtext, sizeof(m.mtext), "%s", text);
    return msgsnd(id, &m, sizeof(m.mtext), flags);
}

static void on_alarm(int sig)
{
    (void) sig;
}

/* Bounds a blocking call in this process: SIGALRM without SA_RESTART turns a
 * wait nobody ends into EINTR, which the caller reports as a failure.
 */
static void arm_deadline(unsigned sec)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(sec);
}

/* The parent removes the queue only after the child is about to block, so a
 * child slow to start does not reach the queue after RMID. The sleep covers the
 * short window left between the child's write and its wait.
 */
static void signal_ready(int fd[2])
{
    close(fd[0]);
    (void) write(fd[1], "r", 1);
    close(fd[1]);
}

static void await_ready(int fd[2])
{
    char c;
    close(fd[1]);
    (void) read(fd[0], &c, 1);
    close(fd[0]);
    sleep_ms(200);
}

/* The receiver asks for type 2 while a type 1 message is already queued, so the
 * wake has to come from the later send and the type filter has to survive the
 * retry loop.
 */
static void test_msgrcv_woken_by_send(void)
{
    TEST("msgrcv blocks until a matching send");
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("msgget");
        return;
    }
    if (send_one(id, 1, "skip", 0) < 0) {
        FAIL("msgsnd type 1");
        msgctl(id, IPC_RMID, NULL);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(200);
        _exit(send_one(id, 2, "wake", 0) == 0 ? 0 : 1);
    }

    struct small_msg m;
    arm_deadline(5);
    ssize_t n = msgrcv(id, &m, sizeof(m.mtext), 2, 0);
    alarm(0);
    int rc = reap_within(pid, 5000);
    if (n != (ssize_t) sizeof(m.mtext) || m.mtype != 2 ||
        strcmp(m.mtext, "wake") != 0)
        FAIL("wrong message");
    else if (rc != 0)
        FAIL("sender failed");
    else if (msgrcv(id, &m, sizeof(m.mtext), 1, IPC_NOWAIT) < 0)
        FAIL("type 1 message lost");
    else if (msgrcv(id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) >= 0 ||
             errno != ENOMSG)
        FAIL("expected ENOMSG on empty queue with IPC_NOWAIT");
    else
        PASS();
    msgctl(id, IPC_RMID, NULL);
}

/* Fill the queue until a nonblocking send is refused. Returns the count. */
static int fill_queue(int id)
{
    int count = 0;
    while (count < 100000 && send_one(id, 1, "fill", IPC_NOWAIT) == 0)
        count++;
    return errno == EAGAIN ? count : -1;
}

static void test_msgsnd_woken_by_recv(void)
{
    TEST("msgsnd on a full queue blocks until a receive");
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("msgget");
        return;
    }
    if (fill_queue(id) <= 0) {
        FAIL("could not fill queue");
        msgctl(id, IPC_RMID, NULL);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(200);
        struct small_msg m;
        _exit(msgrcv(id, &m, sizeof(m.mtext), 0, 0) >= 0 ? 0 : 1);
    }
    arm_deadline(5);
    int sent = send_one(id, 3, "last", 0);
    alarm(0);
    int rc = reap_within(pid, 5000);
    struct small_msg m;
    if (sent < 0)
        FAIL("blocking msgsnd failed");
    else if (rc != 0)
        FAIL("receiver failed");
    else if (msgrcv(id, &m, sizeof(m.mtext), 3, IPC_NOWAIT) < 0)
        FAIL("blocked message not queued");
    else
        PASS();
    msgctl(id, IPC_RMID, NULL);
}

static void test_msgsnd_eidrm(void)
{
    TEST("msgsnd on a full queue sees EIDRM on RMID");
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("msgget");
        return;
    }
    if (fill_queue(id) <= 0) {
        FAIL("could not fill queue");
        msgctl(id, IPC_RMID, NULL);
        return;
    }
    int ready[2];
    if (pipe(ready) < 0) {
        FAIL("pipe");
        msgctl(id, IPC_RMID, NULL);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        signal_ready(ready);
        if (send_one(id, 1, "late", 0) == 0)
            _exit(0);
        _exit(errno == EIDRM ? 1 : 2);
    }
    await_ready(ready);
    msgctl(id, IPC_RMID, NULL);
    if (reap_within(pid, 5000) == 1)
        PASS();
    else
        FAIL("expected EIDRM");
}

static void test_msgrcv_eidrm(void)
{
    TEST("msgrcv sees EIDRM on RMID");
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("msgget");
        return;
    }
    int ready[2];
    if (pipe(ready) < 0) {
        FAIL("pipe");
        msgctl(id, IPC_RMID, NULL);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        signal_ready(ready);
        struct small_msg m;
        if (msgrcv(id, &m, sizeof(m.mtext), 0, 0) >= 0)
            _exit(0);
        _exit(errno == EIDRM ? 1 : 2);
    }
    await_ready(ready);
    msgctl(id, IPC_RMID, NULL);
    if (reap_within(pid, 5000) == 1)
        PASS();
    else
        FAIL("expected EIDRM");
}

/* Linux answers a signal during a msgrcv or msgsnd wait with ERESTARTNOHAND, so
 * a handler, even an SA_RESTART one, sees EINTR rather than a restart.
 */
static void test_msgrcv_eintr(void)
{
    TEST("msgrcv interrupted by SA_RESTART handler gives EINTR");
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("msgget");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alarm;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGALRM, &sa, NULL);
        struct itimerval it = {.it_value = {.tv_usec = 200000}};
        setitimer(ITIMER_REAL, &it, NULL);
        struct small_msg m;
        if (msgrcv(id, &m, sizeof(m.mtext), 0, 0) >= 0)
            _exit(2);
        _exit(errno == EINTR ? 0 : 3);
    }
    int rc = reap_within(pid, 5000);
    if (rc == 0)
        PASS();
    else
        FAIL(rc < 0 ? "msgrcv not interrupted" : "expected EINTR");
    msgctl(id, IPC_RMID, NULL);
}

static void *blocked_receiver(void *arg)
{
    int id = *(int *) arg;
    struct small_msg m;
    msgrcv(id, &m, sizeof(m.mtext), 0, 0);
    return NULL;
}

/* A thread parked in msgrcv must not keep the process alive past a sibling's
 * exit_group.
 */
static void test_exit_group_ends_wait(void)
{
    TEST("exit_group ends a thread blocked in msgrcv");
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("msgget");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t;
        if (pthread_create(&t, NULL, blocked_receiver, &id) != 0)
            _exit(2);
        sleep_ms(200);
        exit(7);
    }
    int rc = reap_within(pid, 4000);
    if (rc == 7)
        PASS();
    else
        FAIL(rc < 0 ? "process did not exit" : "wrong exit status");
    msgctl(id, IPC_RMID, NULL);
}

/* execve tears the sibling down the way Linux de_thread does, and a sibling it
 * cannot reach turns the exec into a fatal exit instead of the new image.
 */
static void test_execve_ends_wait(const char *self)
{
    TEST("execve ends a sibling blocked in msgrcv");
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) {
        FAIL("msgget");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t;
        if (pthread_create(&t, NULL, blocked_receiver, &id) != 0)
            _exit(2);
        sleep_ms(200);
        char *argv[] = {(char *) self, "exec-child", NULL};
        execv(self, argv);
        _exit(3);
    }
    int rc = reap_within(pid, 8000);
    if (rc == 7)
        PASS();
    else
        FAIL(rc < 0 ? "process did not exit" : "execve did not complete");
    msgctl(id, IPC_RMID, NULL);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "exec-child") == 0)
        return 7;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("test-sysv-msg-wait\n");

    test_msgrcv_woken_by_send();
    test_msgsnd_woken_by_recv();
    test_msgsnd_eidrm();
    test_msgrcv_eidrm();
    test_msgrcv_eintr();
    test_exit_group_ends_wait();
    test_execve_ends_wait(argv[0]);

    SUMMARY("test-sysv-msg-wait");
    return fails ? 1 : 0;
}

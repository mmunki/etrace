/*
 * test_crash.c -- the trace fixture used by the etrace tests.
 *
 * It performs file I/O, starts and joins a worker thread, then deliberately
 * dereferences a null pointer.  Recording it produces openat/write/read/
 * clone/futex/exit_group events followed by a SIGSEGV signal event, which is
 * exactly what the replay tests step backwards through.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void *worker(void *arg)
{
    const char *path = "etrace-thread.tmp";
    const char *text = "worker thread file I/O\n";
    char buf[64];
    int fd;

    (void)arg;
    /* A deliberately failing openat gives the trace a real error event. */
    fd = open("/nonexistent/etrace-fixture", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
        close(fd);

    fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ssize_t n = write(fd, text, strlen(text));
        lseek(fd, 0, SEEK_SET);
        if (n > 0 && read(fd, buf, sizeof(buf)) < 0)
            _exit(1);
        close(fd);
    }
    return NULL;
}

int main(void)
{
    const char *path = "etrace-main.tmp";
    const char *text = "main thread file I/O\n";
    char buf[64];
    pthread_t thread;
    int fd;

    fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ssize_t n = write(fd, text, strlen(text));
        lseek(fd, 0, SEEK_SET);
        if (n > 0 && read(fd, buf, sizeof(buf)) < 0)
            return 1;
        close(fd);
    }

    if (pthread_create(&thread, NULL, worker, NULL) == 0)
        pthread_join(thread, NULL);

    printf("etrace test fixture is about to crash on purpose\n");
    fflush(stdout);

    {
        volatile int *bad = (volatile int *)0;
        *bad = 0x1337;
    }

    return 0;
}

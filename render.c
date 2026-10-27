#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "etrace.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

struct sbuf {
    char *buf;
    size_t len;
    size_t off;
};

static void sb_append(struct sbuf *sb, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room;

    if (sb->off >= sb->len)
        return;
    room = sb->len - sb->off;
    va_start(ap, fmt);
    n = vsnprintf(sb->buf + sb->off, room, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= room)
        sb->off = sb->len - 1;
    else
        sb->off += (size_t)n;
}

typedef struct {
    uint64_t value;
    const char *name;
} flag_def_t;

static void sb_flags(struct sbuf *sb, uint64_t value,
                     const flag_def_t *defs, size_t count)
{
    int first = 1;
    size_t i;

    for (i = 0; i < count; i++) {
        if ((value & defs[i].value) == defs[i].value) {
            sb_append(sb, "%s%s", first ? "" : "|", defs[i].name);
            first = 0;
            value &= ~defs[i].value;
        }
    }
    if (first)
        sb_append(sb, "0");
    else if (value)
        sb_append(sb, "|0x%llx", (unsigned long long)value);
}

static void sb_open_flags(struct sbuf *sb, uint64_t flags)
{
    static const flag_def_t defs[] = {
        { O_CREAT, "O_CREAT" },
        { O_EXCL, "O_EXCL" },
        { O_NOCTTY, "O_NOCTTY" },
        { O_TRUNC, "O_TRUNC" },
        { O_APPEND, "O_APPEND" },
        { O_NONBLOCK, "O_NONBLOCK" },
        { O_DSYNC, "O_DSYNC" },
        { O_DIRECT, "O_DIRECT" },
        { O_LARGEFILE, "O_LARGEFILE" },
        { O_DIRECTORY, "O_DIRECTORY" },
        { O_NOFOLLOW, "O_NOFOLLOW" },
        { O_CLOEXEC, "O_CLOEXEC" },
        { O_SYNC, "O_SYNC" },
        { O_PATH, "O_PATH" },
        { O_TMPFILE, "O_TMPFILE" },
    };
    const char *mode;

    switch (flags & O_ACCMODE) {
    case O_WRONLY: mode = "O_WRONLY"; break;
    case O_RDWR: mode = "O_RDWR"; break;
    default: mode = "O_RDONLY"; break;
    }
    sb_append(sb, "%s", mode);
    flags &= ~(uint64_t)O_ACCMODE;
    if (flags)
        sb_append(sb, "|");
    sb_flags(sb, flags, defs, sizeof(defs) / sizeof(defs[0]));
}

static void sb_mmap_prot(struct sbuf *sb, uint64_t prot)
{
    static const flag_def_t defs[] = {
        { PROT_READ, "PROT_READ" },
        { PROT_WRITE, "PROT_WRITE" },
        { PROT_EXEC, "PROT_EXEC" },
        { PROT_GROWSDOWN, "PROT_GROWSDOWN" },
        { PROT_GROWSUP, "PROT_GROWSUP" },
    };
    if (prot == 0) {
        sb_append(sb, "PROT_NONE");
        return;
    }
    sb_flags(sb, prot, defs, sizeof(defs) / sizeof(defs[0]));
}

static void sb_mmap_flags(struct sbuf *sb, uint64_t flags)
{
    static const flag_def_t defs[] = {
        { MAP_FIXED, "MAP_FIXED" },
        { MAP_ANONYMOUS, "MAP_ANONYMOUS" },
#ifdef MAP_32BIT
        { MAP_32BIT, "MAP_32BIT" },
#endif
        { MAP_GROWSDOWN, "MAP_GROWSDOWN" },
        { MAP_DENYWRITE, "MAP_DENYWRITE" },
        { MAP_EXECUTABLE, "MAP_EXECUTABLE" },
        { MAP_LOCKED, "MAP_LOCKED" },
        { MAP_NORESERVE, "MAP_NORESERVE" },
        { MAP_POPULATE, "MAP_POPULATE" },
        { MAP_NONBLOCK, "MAP_NONBLOCK" },
        { MAP_STACK, "MAP_STACK" },
        { MAP_HUGETLB, "MAP_HUGETLB" },
        { MAP_SYNC, "MAP_SYNC" },
    };
    const char *share = NULL;

    switch (flags & MAP_TYPE) {
    case MAP_SHARED: share = "MAP_SHARED"; break;
    case MAP_PRIVATE: share = "MAP_PRIVATE"; break;
    default: break;
    }
    if (share) {
        sb_append(sb, "%s", share);
        flags &= ~(uint64_t)MAP_TYPE;
        if (flags)
            sb_append(sb, "|");
    }
    sb_flags(sb, flags, defs, sizeof(defs) / sizeof(defs[0]));
}

static void sb_clone_flags(struct sbuf *sb, uint64_t flags)
{
    static const flag_def_t defs[] = {
        { CLONE_VM, "CLONE_VM" },
        { CLONE_FS, "CLONE_FS" },
        { CLONE_FILES, "CLONE_FILES" },
        { CLONE_SIGHAND, "CLONE_SIGHAND" },
        { CLONE_PIDFD, "CLONE_PIDFD" },
        { CLONE_PTRACE, "CLONE_PTRACE" },
        { CLONE_VFORK, "CLONE_VFORK" },
        { CLONE_PARENT, "CLONE_PARENT" },
        { CLONE_THREAD, "CLONE_THREAD" },
        { CLONE_NEWNS, "CLONE_NEWNS" },
        { CLONE_SYSVSEM, "CLONE_SYSVSEM" },
        { CLONE_SETTLS, "CLONE_SETTLS" },
        { CLONE_PARENT_SETTID, "CLONE_PARENT_SETTID" },
        { CLONE_CHILD_CLEARTID, "CLONE_CHILD_CLEARTID" },
        { CLONE_CHILD_SETTID, "CLONE_CHILD_SETTID" },
        { CLONE_NEWCGROUP, "CLONE_NEWCGROUP" },
        { CLONE_NEWUTS, "CLONE_NEWUTS" },
        { CLONE_NEWIPC, "CLONE_NEWIPC" },
        { CLONE_NEWUSER, "CLONE_NEWUSER" },
        { CLONE_NEWPID, "CLONE_NEWPID" },
        { CLONE_NEWNET, "CLONE_NEWNET" },
    };
    uint64_t sig = flags & 0xff;

    flags &= ~(uint64_t)0xff;
    sb_flags(sb, flags, defs, sizeof(defs) / sizeof(defs[0]));
    if (sig) {
        char name[32];
        snprintf(name, sizeof(name), "SIG%s", etrace_signal_name((int)sig));
        sb_append(sb, "|%s", name);
    }
}

static void sb_futex_op(struct sbuf *sb, uint64_t op)
{
    static const flag_def_t defs[] = {
        { 128, "FUTEX_PRIVATE_FLAG" },
        { 256, "FUTEX_CLOCK_REALTIME" },
    };
    static const char *const names[] = {
        "FUTEX_WAIT", "FUTEX_WAKE", "FUTEX_FD", "FUTEX_REQUEUE",
        "FUTEX_CMP_REQUEUE", "FUTEX_WAKE_OP", "FUTEX_LOCK_PI",
        "FUTEX_UNLOCK_PI", "FUTEX_TRYLOCK_PI", "FUTEX_WAIT_BITSET",
        "FUTEX_WAKE_BITSET", "FUTEX_WAIT_REQUEUE_PI", "FUTEX_CMP_REQUEUE_PI",
    };
    uint64_t cmd = op & 0x7f;

    if (cmd < sizeof(names) / sizeof(names[0]))
        sb_append(sb, "%s", names[cmd]);
    else
        sb_append(sb, "cmd=%llu", (unsigned long long)cmd);
    if (op & ~(uint64_t)0x7f)
        sb_append(sb, "|");
    sb_flags(sb, op & ~(uint64_t)0x7f, defs, sizeof(defs) / sizeof(defs[0]));
}

static void sb_fd(struct sbuf *sb, uint64_t arg)
{
    int fd = (int)(int32_t)arg;
    if (fd == AT_FDCWD)
        sb_append(sb, "AT_FDCWD");
    else
        sb_append(sb, "%d", fd);
}

static void sb_ll(struct sbuf *sb, uint64_t arg)
{
    sb_append(sb, "%lld", (long long)(int64_t)arg);
}

static void sb_ull(struct sbuf *sb, uint64_t arg)
{
    sb_append(sb, "%llu", (unsigned long long)arg);
}

static void sb_ptr(struct sbuf *sb, uint64_t arg)
{
    sb_append(sb, "0x%llx", (unsigned long long)arg);
}

const char *etrace_signal_name(int sig)
{
    switch (sig) {
    case SIGHUP: return "HUP";
    case SIGINT: return "INT";
    case SIGQUIT: return "QUIT";
    case SIGILL: return "ILL";
    case SIGTRAP: return "TRAP";
    case SIGABRT: return "ABRT";
    case SIGBUS: return "BUS";
    case SIGFPE: return "FPE";
    case SIGKILL: return "KILL";
    case SIGUSR1: return "USR1";
    case SIGSEGV: return "SEGV";
    case SIGUSR2: return "USR2";
    case SIGPIPE: return "PIPE";
    case SIGALRM: return "ALRM";
    case SIGTERM: return "TERM";
    case SIGCHLD: return "CHLD";
    case SIGCONT: return "CONT";
    case SIGSTOP: return "STOP";
    case SIGTSTP: return "TSTP";
    case SIGTTIN: return "TTIN";
    case SIGTTOU: return "TTOU";
    case SIGSYS: return "SYS";
    default: return "UNKNOWN";
    }
}

const char *etrace_signal_description(int sig)
{
    const char *desc = strsignal(sig);
    return desc ? desc : "unknown signal";
}

static int is_name(const char *name, const char *want)
{
    return name && strcmp(name, want) == 0;
}

static int is_zero_arg(const char *name)
{
    static const char *const names[] = {
        "getpid", "gettid", "getppid", "getuid", "geteuid", "getgid",
        "getegid", "getpgrp", "setsid", "sched_yield", "fork", "vfork",
        "pause", "rt_sigreturn", "sync", "getpagesize",
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (is_name(name, names[i]))
            return 1;
    }
    return 0;
}

static void sb_generic_args(struct sbuf *sb, const etrace_event_t *ev)
{
    sb_append(sb, "0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx",
              (unsigned long long)ev->args[0], (unsigned long long)ev->args[1],
              (unsigned long long)ev->args[2], (unsigned long long)ev->args[3],
              (unsigned long long)ev->args[4], (unsigned long long)ev->args[5]);
}

static void sb_call_openat(struct sbuf *sb, const etrace_event_t *ev)
{
    sb_append(sb, "dirfd=");
    sb_fd(sb, ev->args[0]);
    sb_append(sb, ", path=");
    sb_ptr(sb, ev->args[1]);
    sb_append(sb, ", flags=");
    sb_open_flags(sb, ev->args[2]);
    sb_append(sb, ", mode=0%llo", (unsigned long long)ev->args[3]);
}

static void sb_call_open(struct sbuf *sb, const etrace_event_t *ev)
{
    sb_append(sb, "path=");
    sb_ptr(sb, ev->args[0]);
    sb_append(sb, ", flags=");
    sb_open_flags(sb, ev->args[1]);
    sb_append(sb, ", mode=0%llo", (unsigned long long)ev->args[2]);
}

static void sb_call_rw(struct sbuf *sb, const etrace_event_t *ev, const char *name)
{
    sb_append(sb, "fd=%d", (int)(int32_t)ev->args[0]);
    if (is_name(name, "pread64") || is_name(name, "pwrite64")) {
        sb_append(sb, ", buf=");
        sb_ptr(sb, ev->args[1]);
        sb_append(sb, ", count=");
        sb_ull(sb, ev->args[2]);
        sb_append(sb, ", offset=");
        sb_ll(sb, ev->args[3]);
    } else if (is_name(name, "readv") || is_name(name, "writev")) {
        sb_append(sb, ", iov=");
        sb_ptr(sb, ev->args[1]);
        sb_append(sb, ", iovcnt=%d", (int)(int32_t)ev->args[2]);
    } else if (is_name(name, "sendfile")) {
        sb_append(sb, ", out_fd=%d, offset=", (int)(int32_t)ev->args[1]);
        sb_ptr(sb, ev->args[2]);
        sb_append(sb, ", count=");
        sb_ull(sb, ev->args[3]);
    } else {
        sb_append(sb, ", buf=");
        sb_ptr(sb, ev->args[1]);
        sb_append(sb, ", count=");
        sb_ull(sb, ev->args[2]);
    }
}

void etrace_format_call(char *buf, size_t buflen,
                        const etrace_event_t *ev, uint16_t arch)
{
    struct sbuf sb = { buf, buflen, 0 };
    char fallback[48];
    const char *name;

    if (buflen)
        buf[0] = '\0';

    if (ev->syscall_num == ETRACE_SIGNAL_SYSCALL) {
        int sig = (int)ev->args[0];
        sb_append(&sb, "signal SIG%s (%d): %s", etrace_signal_name(sig), sig,
                  etrace_signal_description(sig));
        return;
    }

    name = etrace_syscall_name(arch, ev->syscall_num);
    if (!name) {
        snprintf(fallback, sizeof(fallback), "syscall_%u", ev->syscall_num);
        name = fallback;
    }

    sb_append(&sb, "%s(", name);
    if (is_zero_arg(name)) {
        /* no arguments */
    } else if (is_name(name, "read") || is_name(name, "write") ||
               is_name(name, "pread64") || is_name(name, "pwrite64") ||
               is_name(name, "readv") || is_name(name, "writev") ||
               is_name(name, "sendfile")) {
        sb_call_rw(&sb, ev, name);
    } else if (is_name(name, "openat")) {
        sb_call_openat(&sb, ev);
    } else if (is_name(name, "open")) {
        sb_call_open(&sb, ev);
    } else if (is_name(name, "openat2")) {
        sb_append(&sb, "dirfd=");
        sb_fd(&sb, ev->args[0]);
        sb_append(&sb, ", path=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", how=");
        sb_ptr(&sb, ev->args[2]);
        sb_append(&sb, ", size=");
        sb_ull(&sb, ev->args[3]);
    } else if (is_name(name, "close")) {
        sb_append(&sb, "fd=%d", (int)(int32_t)ev->args[0]);
    } else if (is_name(name, "mmap") || is_name(name, "mmap2")) {
        sb_append(&sb, "addr=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", len=");
        sb_ull(&sb, ev->args[1]);
        sb_append(&sb, ", prot=");
        sb_mmap_prot(&sb, ev->args[2]);
        sb_append(&sb, ", flags=");
        sb_mmap_flags(&sb, ev->args[3]);
        sb_append(&sb, ", fd=%d, off=0x%llx", (int)(int32_t)ev->args[4],
                  (unsigned long long)ev->args[5]);
    } else if (is_name(name, "mprotect")) {
        sb_append(&sb, "addr=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", len=");
        sb_ull(&sb, ev->args[1]);
        sb_append(&sb, ", prot=");
        sb_mmap_prot(&sb, ev->args[2]);
    } else if (is_name(name, "munmap")) {
        sb_append(&sb, "addr=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", len=");
        sb_ull(&sb, ev->args[1]);
    } else if (is_name(name, "brk")) {
        sb_append(&sb, "addr=");
        sb_ptr(&sb, ev->args[0]);
    } else if (is_name(name, "mremap")) {
        sb_append(&sb, "old=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", old_size=");
        sb_ull(&sb, ev->args[1]);
        sb_append(&sb, ", new_size=");
        sb_ull(&sb, ev->args[2]);
        sb_append(&sb, ", flags=0x%llx, new_addr=",
                  (unsigned long long)ev->args[3]);
        sb_ptr(&sb, ev->args[4]);
    } else if (is_name(name, "clone")) {
        sb_append(&sb, "flags=");
        sb_clone_flags(&sb, ev->args[0]);
        sb_append(&sb, ", stack=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", parent_tid=");
        sb_ptr(&sb, ev->args[2]);
        sb_append(&sb, ", child_tid=");
        sb_ptr(&sb, ev->args[3]);
        sb_append(&sb, ", tls=");
        sb_ptr(&sb, ev->args[4]);
    } else if (is_name(name, "clone3")) {
        sb_append(&sb, "args=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", size=");
        sb_ull(&sb, ev->args[1]);
    } else if (is_name(name, "execve")) {
        sb_append(&sb, "path=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", argv=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", envp=");
        sb_ptr(&sb, ev->args[2]);
    } else if (is_name(name, "execveat")) {
        sb_append(&sb, "dirfd=");
        sb_fd(&sb, ev->args[0]);
        sb_append(&sb, ", path=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", argv=");
        sb_ptr(&sb, ev->args[2]);
        sb_append(&sb, ", envp=");
        sb_ptr(&sb, ev->args[3]);
        sb_append(&sb, ", flags=0x%llx", (unsigned long long)ev->args[4]);
    } else if (is_name(name, "exit") || is_name(name, "exit_group")) {
        sb_append(&sb, "status=%d", (int)(int32_t)ev->args[0]);
    } else if (is_name(name, "futex")) {
        sb_append(&sb, "uaddr=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", op=");
        sb_futex_op(&sb, ev->args[1]);
        sb_append(&sb, ", val=");
        sb_ll(&sb, ev->args[2]);
        sb_append(&sb, ", timeout=");
        sb_ptr(&sb, ev->args[3]);
        sb_append(&sb, ", uaddr2=");
        sb_ptr(&sb, ev->args[4]);
        sb_append(&sb, ", val3=");
        sb_ll(&sb, ev->args[5]);
    } else if (is_name(name, "futex_waitv")) {
        sb_append(&sb, "waiters=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", nr_futexes=");
        sb_ull(&sb, ev->args[1]);
        sb_append(&sb, ", flags=0x%llx, timeout=",
                  (unsigned long long)ev->args[2]);
        sb_ptr(&sb, ev->args[3]);
        sb_append(&sb, ", clockid=");
        sb_ll(&sb, ev->args[4]);
    } else if (is_name(name, "ioctl")) {
        sb_append(&sb, "fd=%d, request=0x%llx, arg=0x%llx",
                  (int)(int32_t)ev->args[0],
                  (unsigned long long)ev->args[1],
                  (unsigned long long)ev->args[2]);
    } else if (is_name(name, "fcntl")) {
        sb_append(&sb, "fd=%d, cmd=%d, arg=0x%llx",
                  (int)(int32_t)ev->args[0], (int)(int32_t)ev->args[1],
                  (unsigned long long)ev->args[2]);
    } else if (is_name(name, "lseek")) {
        static const char *const whence[] = {
            "SEEK_SET", "SEEK_CUR", "SEEK_END", "SEEK_DATA", "SEEK_HOLE"
        };
        int w = (int)(int32_t)ev->args[2];
        sb_append(&sb, "fd=%d, offset=%lld, whence=", (int)(int32_t)ev->args[0],
                  (long long)(int64_t)ev->args[1]);
        if (w >= 0 && w < (int)(sizeof(whence) / sizeof(whence[0])))
            sb_append(&sb, "%s", whence[w]);
        else
            sb_append(&sb, "%d", w);
    } else if (is_name(name, "newfstatat")) {
        sb_append(&sb, "dirfd=");
        sb_fd(&sb, ev->args[0]);
        sb_append(&sb, ", path=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", statbuf=");
        sb_ptr(&sb, ev->args[2]);
        sb_append(&sb, ", flags=0x%llx", (unsigned long long)ev->args[3]);
    } else if (is_name(name, "fstat") || is_name(name, "stat") ||
               is_name(name, "lstat")) {
        if (is_name(name, "fstat")) {
            sb_append(&sb, "fd=%d, statbuf=", (int)(int32_t)ev->args[0]);
            sb_ptr(&sb, ev->args[1]);
        } else {
            sb_append(&sb, "path=");
            sb_ptr(&sb, ev->args[0]);
            sb_append(&sb, ", statbuf=");
            sb_ptr(&sb, ev->args[1]);
        }
    } else if (is_name(name, "statx")) {
        sb_append(&sb, "dirfd=");
        sb_fd(&sb, ev->args[0]);
        sb_append(&sb, ", path=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", flags=0x%llx, mask=0x%llx, statxbuf=",
                  (unsigned long long)ev->args[2],
                  (unsigned long long)ev->args[3]);
        sb_ptr(&sb, ev->args[4]);
    } else if (is_name(name, "access")) {
        sb_append(&sb, "path=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", mode=0x%llx", (unsigned long long)ev->args[1]);
    } else if (is_name(name, "faccessat") || is_name(name, "faccessat2")) {
        sb_append(&sb, "dirfd=");
        sb_fd(&sb, ev->args[0]);
        sb_append(&sb, ", path=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", mode=0x%llx", (unsigned long long)ev->args[2]);
        if (is_name(name, "faccessat2"))
            sb_append(&sb, ", flags=0x%llx",
                      (unsigned long long)ev->args[3]);
    } else if (is_name(name, "unlink")) {
        sb_append(&sb, "path=");
        sb_ptr(&sb, ev->args[0]);
    } else if (is_name(name, "unlinkat")) {
        sb_append(&sb, "dirfd=");
        sb_fd(&sb, ev->args[0]);
        sb_append(&sb, ", path=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", flags=0x%llx", (unsigned long long)ev->args[2]);
    } else if (is_name(name, "mkdir")) {
        sb_append(&sb, "path=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", mode=0%llo", (unsigned long long)ev->args[1]);
    } else if (is_name(name, "mkdirat")) {
        sb_append(&sb, "dirfd=");
        sb_fd(&sb, ev->args[0]);
        sb_append(&sb, ", path=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", mode=0%llo", (unsigned long long)ev->args[2]);
    } else if (is_name(name, "rename")) {
        sb_append(&sb, "old=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", new=");
        sb_ptr(&sb, ev->args[1]);
    } else if (is_name(name, "renameat") || is_name(name, "renameat2")) {
        sb_append(&sb, "olddirfd=");
        sb_fd(&sb, ev->args[0]);
        sb_append(&sb, ", old=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", newdirfd=");
        sb_fd(&sb, ev->args[2]);
        sb_append(&sb, ", new=");
        sb_ptr(&sb, ev->args[3]);
        if (is_name(name, "renameat2"))
            sb_append(&sb, ", flags=0x%llx",
                      (unsigned long long)ev->args[4]);
    } else if (is_name(name, "getcwd")) {
        sb_append(&sb, "buf=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", size=");
        sb_ull(&sb, ev->args[1]);
    } else if (is_name(name, "dup")) {
        sb_append(&sb, "oldfd=%d", (int)(int32_t)ev->args[0]);
    } else if (is_name(name, "dup3")) {
        sb_append(&sb, "oldfd=%d, newfd=%d, flags=0x%llx",
                  (int)(int32_t)ev->args[0], (int)(int32_t)ev->args[1],
                  (unsigned long long)ev->args[2]);
    } else if (is_name(name, "pipe") || is_name(name, "pipe2")) {
        sb_append(&sb, "pipefd=");
        sb_ptr(&sb, ev->args[0]);
        if (is_name(name, "pipe2"))
            sb_append(&sb, ", flags=0x%llx",
                      (unsigned long long)ev->args[1]);
    } else if (is_name(name, "poll")) {
        sb_append(&sb, "fds=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", nfds=");
        sb_ull(&sb, ev->args[1]);
        sb_append(&sb, ", timeout=%d", (int)(int32_t)ev->args[2]);
    } else if (is_name(name, "ppoll")) {
        sb_append(&sb, "fds=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", nfds=");
        sb_ull(&sb, ev->args[1]);
        sb_append(&sb, ", timeout=");
        sb_ptr(&sb, ev->args[2]);
        sb_append(&sb, ", sigmask=");
        sb_ptr(&sb, ev->args[3]);
        sb_append(&sb, ", sigsetsize=");
        sb_ull(&sb, ev->args[4]);
    } else if (is_name(name, "epoll_ctl")) {
        static const char *const ops[] = { "EPOLL_CTL_ADD", "EPOLL_CTL_DEL", "EPOLL_CTL_MOD" };
        int op = (int)(int32_t)ev->args[1];
        sb_append(&sb, "epfd=%d, op=", (int)(int32_t)ev->args[0]);
        if (op >= 0 && op < 3)
            sb_append(&sb, "%s", ops[op]);
        else
            sb_append(&sb, "%d", op);
        sb_append(&sb, ", fd=%d, event=", (int)(int32_t)ev->args[2]);
        sb_ptr(&sb, ev->args[3]);
    } else if (is_name(name, "epoll_wait") || is_name(name, "epoll_pwait") ||
               is_name(name, "epoll_pwait2")) {
        sb_append(&sb, "epfd=%d, events=", (int)(int32_t)ev->args[0]);
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", maxevents=%d, timeout=%d",
                  (int)(int32_t)ev->args[2], (int)(int32_t)ev->args[3]);
        if (!is_name(name, "epoll_wait"))
            sb_append(&sb, ", sigmask=");
        if (!is_name(name, "epoll_wait") && !is_name(name, "epoll_pwait2"))
            sb_ptr(&sb, ev->args[4]);
    } else if (is_name(name, "socket")) {
        sb_append(&sb, "domain=%d, type=0x%llx, protocol=%d",
                  (int)(int32_t)ev->args[0],
                  (unsigned long long)ev->args[1],
                  (int)(int32_t)ev->args[2]);
    } else if (is_name(name, "connect") || is_name(name, "bind")) {
        sb_append(&sb, "fd=%d, addr=", (int)(int32_t)ev->args[0]);
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", addrlen=%u", (unsigned)ev->args[2]);
    } else if (is_name(name, "listen")) {
        sb_append(&sb, "fd=%d, backlog=%d", (int)(int32_t)ev->args[0],
                  (int)(int32_t)ev->args[1]);
    } else if (is_name(name, "accept") || is_name(name, "accept4")) {
        sb_append(&sb, "fd=%d, addr=", (int)(int32_t)ev->args[0]);
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", addrlen=");
        sb_ptr(&sb, ev->args[2]);
        if (is_name(name, "accept4"))
            sb_append(&sb, ", flags=0x%llx",
                      (unsigned long long)ev->args[3]);
    } else if (is_name(name, "gettimeofday")) {
        sb_append(&sb, "tv=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", tz=");
        sb_ptr(&sb, ev->args[1]);
    } else if (is_name(name, "clock_gettime") || is_name(name, "clock_getres")) {
        sb_append(&sb, "clockid=%d, tp=", (int)(int32_t)ev->args[0]);
        sb_ptr(&sb, ev->args[1]);
    } else if (is_name(name, "nanosleep")) {
        sb_append(&sb, "req=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", rem=");
        sb_ptr(&sb, ev->args[1]);
    } else if (is_name(name, "clock_nanosleep")) {
        sb_append(&sb, "clockid=%d, flags=0x%llx, req=",
                  (int)(int32_t)ev->args[0],
                  (unsigned long long)ev->args[1]);
        sb_ptr(&sb, ev->args[2]);
        sb_append(&sb, ", rem=");
        sb_ptr(&sb, ev->args[3]);
    } else if (is_name(name, "wait4")) {
        sb_append(&sb, "pid=%d, status=", (int)(int32_t)ev->args[0]);
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", options=0x%llx, rusage=",
                  (unsigned long long)ev->args[2]);
        sb_ptr(&sb, ev->args[3]);
    } else if (is_name(name, "madvise")) {
        sb_append(&sb, "addr=");
        sb_ptr(&sb, ev->args[0]);
        sb_append(&sb, ", length=");
        sb_ull(&sb, ev->args[1]);
        sb_append(&sb, ", advice=%d", (int)(int32_t)ev->args[2]);
    } else if (is_name(name, "membarrier")) {
        sb_append(&sb, "cmd=%d, flags=0x%llx, cpu_id=%d",
                  (int)(int32_t)ev->args[0],
                  (unsigned long long)ev->args[1],
                  (int)(int32_t)ev->args[2]);
    } else if (is_name(name, "eventfd2")) {
        sb_append(&sb, "initval=%u, flags=0x%llx",
                  (unsigned)ev->args[0],
                  (unsigned long long)ev->args[1]);
    } else if (is_name(name, "getdents64")) {
        sb_append(&sb, "fd=%d, dirp=", (int)(int32_t)ev->args[0]);
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", count=");
        sb_ull(&sb, ev->args[2]);
    } else if (is_name(name, "prctl")) {
        sb_append(&sb, "option=%d, arg2=0x%llx, arg3=0x%llx, arg4=0x%llx, arg5=0x%llx",
                  (int)(int32_t)ev->args[0],
                  (unsigned long long)ev->args[1],
                  (unsigned long long)ev->args[2],
                  (unsigned long long)ev->args[3],
                  (unsigned long long)ev->args[4]);
    } else if (is_name(name, "arch_prctl")) {
        sb_append(&sb, "code=0x%llx, addr=0x%llx",
                  (unsigned long long)ev->args[0],
                  (unsigned long long)ev->args[1]);
    } else if (is_name(name, "rt_sigaction")) {
        sb_append(&sb, "signum=%d, act=", (int)(int32_t)ev->args[0]);
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", oldact=");
        sb_ptr(&sb, ev->args[2]);
        sb_append(&sb, ", sigsetsize=");
        sb_ull(&sb, ev->args[3]);
    } else if (is_name(name, "rt_sigprocmask")) {
        static const char *const how[] = { "SIG_BLOCK", "SIG_UNBLOCK", "SIG_SETMASK" };
        int h = (int)(int32_t)ev->args[0];
        if (h >= 0 && h < 3)
            sb_append(&sb, "how=%s", how[h]);
        else
            sb_append(&sb, "how=%d", h);
        sb_append(&sb, ", set=");
        sb_ptr(&sb, ev->args[1]);
        sb_append(&sb, ", oldset=");
        sb_ptr(&sb, ev->args[2]);
        sb_append(&sb, ", sigsetsize=");
        sb_ull(&sb, ev->args[3]);
    } else if (is_name(name, "tgkill")) {
        sb_append(&sb, "tgid=%d, tid=%d, sig=%d",
                  (int)(int32_t)ev->args[0], (int)(int32_t)ev->args[1],
                  (int)(int32_t)ev->args[2]);
    } else if (is_name(name, "kill")) {
        sb_append(&sb, "pid=%d, sig=%d", (int)(int32_t)ev->args[0],
                  (int)(int32_t)ev->args[1]);
    } else {
        sb_generic_args(&sb, ev);
    }
    sb_append(&sb, ")");
}

static int returns_pointer(const char *name)
{
    return is_name(name, "mmap") || is_name(name, "mmap2") ||
           is_name(name, "brk") || is_name(name, "mremap") ||
           is_name(name, "shmat") || is_name(name, "getcwd");
}

void etrace_format_ret(char *buf, size_t buflen,
                       const etrace_event_t *ev, uint16_t arch)
{
    struct sbuf sb = { buf, buflen, 0 };
    const char *name;

    if (buflen)
        buf[0] = '\0';
    if (ev->syscall_num == ETRACE_SIGNAL_SYSCALL)
        return;

    name = etrace_syscall_name(arch, ev->syscall_num);
    if (ev->ret_val < 0 && ev->ret_val >= -4095) {
        int err = (int)-ev->ret_val;
        sb_append(&sb, " = -%d (%s)", err, strerror(err));
    } else if (name && returns_pointer(name)) {
        sb_append(&sb, " = 0x%llx", (unsigned long long)ev->ret_val);
    } else {
        sb_append(&sb, " = %lld", (long long)ev->ret_val);
    }
}

void etrace_print_relative_time(FILE *out, uint64_t ns, uint64_t base_ns)
{
    uint64_t delta = ns >= base_ns ? ns - base_ns : 0;
    fprintf(out, "+%llu.%09llu", (unsigned long long)(delta / 1000000000ull),
            (unsigned long long)(delta % 1000000000ull));
}

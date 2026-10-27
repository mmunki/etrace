#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "etrace.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <sys/user.h>
#elif defined(__aarch64__)
#include <asm/ptrace.h>
#include <elf.h>
#include <sys/uio.h>
#endif

#if defined(__has_include)
#if __has_include(<linux/ptrace.h>)
#include <linux/ptrace.h>
#endif
#endif

#define ETRACE_WRITE_BUFFER (1024u * 1024u)

typedef struct {
    int valid;
    uint64_t timestamp_ns;
    uint32_t pid;
    uint32_t tid;
    uint32_t nr;
    uint64_t args[6];
} pending_syscall_t;

typedef struct thread_state {
    pid_t tid;
    uint32_t pid;
    pending_syscall_t pending;
    int in_syscall;
    int options_set;
    int new_thread;
    struct thread_state *next;
} thread_state_t;

typedef struct {
    FILE *fp;
    etrace_header_t header;
    uint64_t count;
    int failed;
    int quiet;
    const char *path;
} writer_t;

static thread_state_t *threads;
static thread_state_t *last_thread;
static unsigned long threads_seen;

static int read_tgid(pid_t tid)
{
    char path[64];
    char line[256];
    FILE *fp;
    int tgid = (int)tid;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)tid);
    fp = fopen(path, "r");
    if (!fp)
        return tgid;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Tgid:", 5) == 0) {
            tgid = atoi(line + 5);
            break;
        }
    }
    fclose(fp);
    return tgid;
}

static thread_state_t *get_thread(pid_t tid)
{
    thread_state_t *ts;

    if (last_thread && last_thread->tid == tid)
        return last_thread;
    for (ts = threads; ts; ts = ts->next) {
        if (ts->tid == tid) {
            last_thread = ts;
            return ts;
        }
    }
    ts = calloc(1, sizeof(*ts));
    if (!ts)
        return NULL;
    ts->tid = tid;
    ts->pid = (uint32_t)read_tgid(tid);
    ts->new_thread = 1;
    ts->next = threads;
    threads = ts;
    last_thread = ts;
    threads_seen++;
    return ts;
}

static void remove_thread(pid_t tid)
{
    thread_state_t **link = &threads;

    while (*link) {
        if ((*link)->tid == tid) {
            thread_state_t *dead = *link;
            *link = dead->next;
            if (last_thread == dead)
                last_thread = NULL;
            free(dead);
            return;
        }
        link = &(*link)->next;
    }
}

static int write_event(writer_t *w, const etrace_event_t *ev)
{
    if (w->failed)
        return -1;
    if (fwrite(ev, sizeof(*ev), 1, w->fp) != 1) {
        w->failed = 1;
        fprintf(stderr, "etrace: cannot write %s: %s\n", w->path,
                strerror(errno));
        return -1;
    }
    w->count++;
    return 0;
}

static void record_signal(writer_t *w, thread_state_t *ts, int sig)
{
    etrace_event_t ev;

    if (sig == SIGSTOP || sig == SIGTRAP || sig == SIGCHLD ||
        sig == SIGCONT || sig == SIGURG || sig == SIGWINCH)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.timestamp_ns = etrace_now_ns();
    ev.pid = ts->pid;
    ev.tid = (uint32_t)ts->tid;
    ev.syscall_num = ETRACE_SIGNAL_SYSCALL;
    ev.ret_val = -sig;
    ev.args[0] = (uint64_t)(unsigned)sig;
    write_event(w, &ev);
}

#if defined(__x86_64__) || defined(__aarch64__)
static int get_regs(pid_t tid, uint32_t *nr, uint64_t args[6], int64_t *ret)
{
#if defined(__x86_64__)
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) != 0)
        return -1;
    *nr = (uint32_t)regs.orig_rax;
    args[0] = regs.rdi;
    args[1] = regs.rsi;
    args[2] = regs.rdx;
    args[3] = regs.r10;
    args[4] = regs.r8;
    args[5] = regs.r9;
    *ret = (int64_t)regs.rax;
    return 0;
#else
    struct user_pt_regs regs;
    struct iovec iov;
    memset(&regs, 0, sizeof(regs));
    iov.iov_base = &regs;
    iov.iov_len = sizeof(regs);
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(unsigned long)NT_PRSTATUS,
               &iov) != 0)
        return -1;
    *nr = (uint32_t)regs.regs[8];
    args[0] = regs.regs[0];
    args[1] = regs.regs[1];
    args[2] = regs.regs[2];
    args[3] = regs.regs[3];
    args[4] = regs.regs[4];
    args[5] = regs.regs[5];
    *ret = (int64_t)regs.regs[0];
    return 0;
#endif
}
#endif

static void pending_entry(thread_state_t *ts, uint32_t nr, const uint64_t args[6])
{
    ts->pending.valid = 1;
    ts->pending.timestamp_ns = etrace_now_ns();
    ts->pending.pid = ts->pid;
    ts->pending.tid = (uint32_t)ts->tid;
    ts->pending.nr = nr;
    memcpy(ts->pending.args, args, sizeof(ts->pending.args));
}

static void pending_exit(writer_t *w, thread_state_t *ts, int64_t ret)
{
    etrace_event_t ev;

    if (!ts->pending.valid)
        return;
    memset(&ev, 0, sizeof(ev));
    ev.timestamp_ns = ts->pending.timestamp_ns;
    ev.pid = ts->pending.pid;
    ev.tid = ts->pending.tid;
    ev.syscall_num = ts->pending.nr;
    ev.ret_val = ret;
    memcpy(ev.args, ts->pending.args, sizeof(ev.args));
    write_event(w, &ev);
    ts->pending.valid = 0;
}

static void handle_syscall_stop(writer_t *w, thread_state_t *ts)
{
#if defined(PTRACE_GET_SYSCALL_INFO)
    struct ptrace_syscall_info info;
    long got;

    memset(&info, 0, sizeof(info));
    got = ptrace(PTRACE_GET_SYSCALL_INFO, ts->tid,
                 (void *)(unsigned long)sizeof(info), &info);
    if (got >= 0) {
        if (info.op == PTRACE_SYSCALL_INFO_ENTRY) {
            uint64_t args[6];
            int i;
            for (i = 0; i < 6; i++)
                args[i] = info.entry.args[i];
            ts->in_syscall = 1;
            pending_entry(ts, (uint32_t)info.entry.nr, args);
            return;
        }
        if (info.op == PTRACE_SYSCALL_INFO_EXIT) {
            ts->in_syscall = 0;
            pending_exit(w, ts, (int64_t)info.exit.rval);
            return;
        }
        if (info.op == PTRACE_SYSCALL_INFO_SECCOMP) {
            uint64_t args[6];
            int i;
            for (i = 0; i < 6; i++)
                args[i] = info.seccomp.args[i];
            ts->in_syscall = 1;
            pending_entry(ts, (uint32_t)info.seccomp.nr, args);
            return;
        }
    }
#endif

#if defined(__x86_64__) || defined(__aarch64__)
    {
        uint32_t nr = 0;
        uint64_t args[6] = {0};
        int64_t ret = 0;
        if (get_regs(ts->tid, &nr, args, &ret) != 0)
            return;
        if (!ts->in_syscall) {
            ts->in_syscall = 1;
            pending_entry(ts, nr, args);
        } else {
            ts->in_syscall = 0;
            pending_exit(w, ts, ret);
        }
    }
#endif
}

static int set_options(pid_t tid, long options)
{
    return ptrace(PTRACE_SETOPTIONS, tid, NULL, (void *)options) == 0 ? 0 : -1;
}

static void usage_record(FILE *out)
{
    fprintf(out,
            "usage: etrace record [options] [--] <command> [args...]\n"
            "\n"
            "options:\n"
            "  -o, --output FILE   write the trace to FILE (default: trace.etrace)\n"
            "      --no-follow     do not follow clone/fork/vfork children\n"
            "      --quiet         do not print a summary\n"
            "  -h, --help          show this help\n");
}

int etrace_record(int argc, char **argv)
{
    const char *output = "trace.etrace";
    int quiet = 0;
    int follow = 1;
    int command_index = -1;
    int i;
    int status = 0;
    pid_t child;
    long options;
    writer_t w;
    etrace_header_t *placeholder;
    int target_status = 0;
    uint64_t started;

    for (i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) {
            command_index = i + 1;
            break;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage_record(stdout);
            return 0;
        }
        if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "etrace: %s needs a value\n", arg);
                return 2;
            }
            output = argv[++i];
            continue;
        }
        if (strncmp(arg, "--output=", 9) == 0) {
            output = arg + 9;
            continue;
        }
        if (strcmp(arg, "--quiet") == 0) {
            quiet = 1;
            continue;
        }
        if (strcmp(arg, "--no-follow") == 0) {
            follow = 0;
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "etrace: unknown record option: %s\n", arg);
            usage_record(stderr);
            return 2;
        }
        command_index = i;
        break;
    }

    if (command_index < 0 || command_index >= argc) {
        fprintf(stderr, "etrace: record needs a command to run\n");
        usage_record(stderr);
        return 2;
    }

    memset(&w, 0, sizeof(w));
    w.path = output;
    w.quiet = quiet;
    w.fp = fopen(output, "wb");
    if (!w.fp) {
        fprintf(stderr, "etrace: cannot create %s: %s\n", output,
                strerror(errno));
        return 1;
    }
    setvbuf(w.fp, NULL, _IOFBF, ETRACE_WRITE_BUFFER);

    memset(&w.header, 0, sizeof(w.header));
    w.header.magic = ETRACE_MAGIC;
    w.header.version = ETRACE_VERSION;
    w.header.arch = (uint16_t)etrace_arch_id();
    w.header.header_size = sizeof(etrace_header_t);
    w.header.event_size = sizeof(etrace_event_t);
    placeholder = &w.header;
    if (fwrite(placeholder, sizeof(*placeholder), 1, w.fp) != 1) {
        fprintf(stderr, "etrace: cannot write %s: %s\n", output,
                strerror(errno));
        fclose(w.fp);
        return 1;
    }

    started = etrace_now_ns();
    w.header.start_ns = started;

    child = fork();
    if (child < 0) {
        fprintf(stderr, "etrace: fork: %s\n", strerror(errno));
        fclose(w.fp);
        return 1;
    }
    if (child == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        execvp(argv[command_index], &argv[command_index]);
        fprintf(stderr, "etrace: exec %s: %s\n", argv[command_index],
                strerror(errno));
        _exit(127);
    }

    if (waitpid(child, &status, 0) < 0 || !WIFSTOPPED(status)) {
        fprintf(stderr, "etrace: target did not stop for tracing: %s\n",
                strerror(errno));
        fclose(w.fp);
        return 1;
    }

    options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE |
              PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
              PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
    if (!follow)
        options &= ~(PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK |
                     PTRACE_O_TRACEVFORK);
    if (set_options(child, options) != 0) {
        fprintf(stderr, "etrace: PTRACE_SETOPTIONS: %s\n", strerror(errno));
        ptrace(PTRACE_DETACH, child, NULL, NULL);
        fclose(w.fp);
        return 1;
    }
    get_thread(child)->options_set = 1;
    if (ptrace(PTRACE_SYSCALL, child, NULL, NULL) != 0) {
        fprintf(stderr, "etrace: PTRACE_SYSCALL: %s\n", strerror(errno));
        fclose(w.fp);
        return 1;
    }

    for (;;) {
        pid_t tid;
        int event;

        tid = waitpid(-1, &status, __WALL);
        if (tid < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ECHILD)
                break;
            fprintf(stderr, "etrace: waitpid: %s\n", strerror(errno));
            break;
        }

        if (WIFEXITED(status)) {
            if (tid == child)
                target_status = WEXITSTATUS(status);
            remove_thread(tid);
            continue;
        }
        if (WIFSIGNALED(status)) {
            if (tid == child)
                target_status = 128 + WTERMSIG(status);
            remove_thread(tid);
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;

        {
            int sig = WSTOPSIG(status);
            event = (unsigned)status >> 16;
            thread_state_t *ts = get_thread(tid);
            if (!ts)
                break;

            if (!ts->options_set) {
                if (set_options(tid, options) == 0)
                    ts->options_set = 1;
            }

            if (sig == (SIGTRAP | 0x80)) {
                handle_syscall_stop(&w, ts);
                ptrace(PTRACE_SYSCALL, tid, NULL, NULL);
            } else if (sig == SIGTRAP && event) {
                if (event == PTRACE_EVENT_CLONE ||
                    event == PTRACE_EVENT_FORK ||
                    event == PTRACE_EVENT_VFORK) {
                    unsigned long new_tid = 0;
                    ptrace(PTRACE_GETEVENTMSG, tid, NULL, &new_tid);
                }
                ptrace(PTRACE_SYSCALL, tid, NULL, NULL);
            } else if (sig == SIGSTOP && ts->new_thread) {
                ts->new_thread = 0;
                ptrace(PTRACE_SYSCALL, tid, NULL, NULL);
            } else {
                record_signal(&w, ts, sig);
                ptrace(PTRACE_SYSCALL, tid, NULL,
                       (void *)(unsigned long)sig);
            }
        }
    }

    w.header.end_ns = etrace_now_ns();
    w.header.event_count = w.count;
    if (fflush(w.fp) != 0)
        w.failed = 1;
    if (!w.failed && fseeko(w.fp, 0, SEEK_SET) == 0 &&
        fwrite(&w.header, sizeof(w.header), 1, w.fp) == 1) {
        /* header updated */
    } else {
        w.failed = 1;
    }
    if (fclose(w.fp) != 0)
        w.failed = 1;

    if (w.failed) {
        fprintf(stderr, "etrace: recording failed, %s is incomplete\n", output);
        return 1;
    }

    if (!quiet) {
        uint64_t duration = w.header.end_ns - w.header.start_ns;
        fprintf(stderr,
                "etrace: recorded %llu events from %lu thread(s) in %llu.%09llu s "
                "(%s) -> %s\n",
                (unsigned long long)w.count, threads_seen,
                (unsigned long long)(duration / 1000000000ull),
                (unsigned long long)(duration % 1000000000ull),
                etrace_arch_name(w.header.arch), output);
        if (target_status > 128) {
            int sig = target_status - 128;
            fprintf(stderr, "etrace: target terminated by signal %d (SIG%s): %s\n",
                    sig, etrace_signal_name(sig),
                    etrace_signal_description(sig));
        }
    }

    return target_status;
}

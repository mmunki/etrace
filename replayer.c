#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "etrace.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const etrace_trace_t *trace;
    const char *path;
    size_t *order; /* timeline step -> record index, sorted by timestamp */
    size_t count;
    size_t current;
} replayer_t;

static int cmp_timeline(const void *a, const void *b, void *arg)
{
    const replayer_t *r = arg;
    size_t ia = *(const size_t *)a;
    size_t ib = *(const size_t *)b;
    const etrace_event_t *ea = &r->trace->events[ia];
    const etrace_event_t *eb = &r->trace->events[ib];

    if (ea->timestamp_ns < eb->timestamp_ns)
        return -1;
    if (ea->timestamp_ns > eb->timestamp_ns)
        return 1;
    if (ia < ib)
        return -1;
    if (ia > ib)
        return 1;
    return 0;
}

static const etrace_event_t *event_at(const replayer_t *r, size_t step)
{
    return &r->trace->events[r->order[step]];
}

static size_t thread_events(const replayer_t *r, uint32_t tid,
                            size_t *first_step, size_t *last_step)
{
    size_t i, n = 0;
    int seen = 0;
    for (i = 0; i < r->count; i++) {
        const etrace_event_t *ev = event_at(r, i);
        if (ev->tid != tid)
            continue;
        if (!seen) {
            if (first_step)
                *first_step = i;
            seen = 1;
        }
        if (last_step)
            *last_step = i;
        n++;
    }
    return n;
}

static void print_event_line(const replayer_t *r, size_t step)
{
    const etrace_event_t *ev = event_at(r, step);
    char call[1024];
    char ret[160];

    etrace_format_call(call, sizeof(call), ev, r->trace->header.arch);
    etrace_format_ret(ret, sizeof(ret), ev, r->trace->header.arch);
    printf("[%6zu] ", step);
    etrace_print_relative_time(stdout, ev->timestamp_ns,
                               r->trace->header.start_ns);
    printf(" pid=%u tid=%u %s%s\n", ev->pid, ev->tid, call, ret);
}

static void print_header(const etrace_trace_t *trace, const char *path)
{
    uint64_t duration = trace->header.end_ns >= trace->header.start_ns
                            ? trace->header.end_ns - trace->header.start_ns
                            : 0;
    printf("Trace:   %s\n", path);
    printf("Format:  ETRACE v%u, %s, %zu events\n",
           (unsigned)trace->header.version,
           etrace_arch_name(trace->header.arch), trace->count);
    printf("Span:    %llu.%09llu s\n",
           (unsigned long long)(duration / 1000000000ull),
           (unsigned long long)(duration % 1000000000ull));
}

static void print_info(const replayer_t *r)
{
    const etrace_event_t *ev;
    char call[1024];
    char ret[160];
    const char *name;
    size_t first_step = 0, last_step = 0;
    size_t n;
    int i;

    if (r->count == 0) {
        printf("The trace has no events.\n");
        return;
    }

    ev = event_at(r, r->current);
    name = ev->syscall_num == ETRACE_SIGNAL_SYSCALL
               ? "signal"
               : etrace_syscall_name(r->trace->header.arch, ev->syscall_num);

    etrace_format_call(call, sizeof(call), ev, r->trace->header.arch);
    etrace_format_ret(ret, sizeof(ret), ev, r->trace->header.arch);
    n = thread_events(r, ev->tid, &first_step, &last_step);

    printf("Step     %zu of %zu (record #%zu)\n", r->current,
           r->count ? r->count - 1 : 0, r->order[r->current]);
    printf("Time     ");
    etrace_print_relative_time(stdout, ev->timestamp_ns,
                               r->trace->header.start_ns);
    printf(" s   absolute %llu ns\n", (unsigned long long)ev->timestamp_ns);
    printf("Thread   pid=%u tid=%u (%zu event%s, steps %zu..%zu)\n",
           ev->pid, ev->tid, n, n == 1 ? "" : "s", first_step, last_step);
    if (ev->syscall_num == ETRACE_SIGNAL_SYSCALL) {
        printf("Event    signal #%u\n", (unsigned)ev->args[0]);
    } else {
        printf("Syscall  %s (#%u)\n", name ? name : "(unknown)",
               ev->syscall_num);
    }
    printf("Args     ");
    if (r->trace->header.arch == ETRACE_ARCH_AARCH64) {
        static const char *const regs[] = { "x0", "x1", "x2", "x3", "x4", "x5" };
        for (i = 0; i < 6; i++)
            printf("%s%s=0x%llx", i ? " " : "", regs[i],
                   (unsigned long long)ev->args[i]);
    } else {
        static const char *const regs[] = { "rdi", "rsi", "rdx", "r10", "r8", "r9" };
        for (i = 0; i < 6; i++)
            printf("%s%s=0x%llx", i ? " " : "", regs[i],
                   (unsigned long long)ev->args[i]);
    }
    printf("\nReturn   %lld\n", (long long)ev->ret_val);
    printf("Call     %s%s\n", call, ret);
}

static void print_threads(const replayer_t *r)
{
    uint32_t *tids = NULL;
    size_t n = 0, cap = 0, i;

    for (i = 0; i < r->count; i++) {
        const etrace_event_t *ev = event_at(r, i);
        size_t j;
        int found = 0;
        for (j = 0; j < n; j++) {
            if (tids[j] == ev->tid) {
                found = 1;
                break;
            }
        }
        if (found)
            continue;
        if (n == cap) {
            size_t next = cap ? cap * 2 : 8;
            uint32_t *grown = realloc(tids, next * sizeof(*tids));
            if (!grown) {
                free(tids);
                printf("out of memory\n");
                return;
            }
            tids = grown;
            cap = next;
        }
        tids[n++] = ev->tid;
    }

    printf("%zu thread(s)\n", n);
    for (i = 0; i < n; i++) {
        size_t first = 0, last = 0;
        size_t count = thread_events(r, tids[i], &first, &last);
        uint32_t pid = 0;
        size_t j;
        for (j = 0; j < r->count; j++) {
            const etrace_event_t *ev = event_at(r, j);
            if (ev->tid == tids[i]) {
                pid = ev->pid;
                break;
            }
        }
        printf("  pid=%u tid=%u events=%zu steps=%zu..%zu\n",
               pid, tids[i], count, first, last);
    }
    free(tids);
}

static void print_list(const replayer_t *r, long radius)
{
    size_t start, end, i;
    if (r->count == 0) {
        printf("The trace has no events.\n");
        return;
    }
    if (radius < 0)
        radius = 0;
    start = r->current > (size_t)radius ? r->current - (size_t)radius : 0;
    end = r->current + (size_t)radius;
    if (end >= r->count)
        end = r->count - 1;
    for (i = start; i <= end; i++) {
        if (i == r->current)
            printf(">");
        else
            printf(" ");
        print_event_line(r, i);
    }
}

static int parse_long(const char *s, long *out)
{
    char *end = NULL;
    long value;
    if (!s || !*s)
        return -1;
    errno = 0;
    value = strtol(s, &end, 0);
    if (errno || !end || *end != '\0')
        return -1;
    *out = value;
    return 0;
}

static void step_relative(replayer_t *r, long delta)
{
    long target;

    if (r->count == 0)
        return;
    target = (long)r->current + delta;
    if (target < 0)
        target = 0;
    if (target >= (long)r->count) {
        target = (long)r->count - 1;
        printf("end of trace: step %ld\n", target);
    } else {
        r->current = (size_t)target;
    }
    r->current = (size_t)target;
    print_event_line(r, r->current);
}

static void step_goto(replayer_t *r, const char *arg)
{
    long target;

    if (r->count == 0) {
        printf("The trace has no events.\n");
        return;
    }
    if (parse_long(arg, &target) != 0) {
        printf("usage: goto <step>, goto +N or goto -N\n");
        return;
    }
    if (arg[0] == '+' || arg[0] == '-')
        target += (long)r->current;
    if (target < 0 || target >= (long)r->count) {
        printf("step out of range: 0..%zu\n", r->count - 1);
        return;
    }
    r->current = (size_t)target;
    print_event_line(r, r->current);
}

static int match_event(const replayer_t *r, const etrace_event_t *ev,
                       uint32_t nr, int mode)
{
    if (mode == 1)
        return ev->syscall_num == ETRACE_SIGNAL_SYSCALL;
    if (mode == 2)
        return ev->ret_val < 0 && ev->syscall_num != ETRACE_SIGNAL_SYSCALL;
    (void)r;
    return ev->syscall_num == nr;
}

static void find_event(replayer_t *r, const char *query)
{
    uint32_t nr = 0;
    int mode = 0;
    size_t *matches = NULL;
    size_t count = 0, capacity = 0;
    size_t i, chosen = 0;
    int have_choice = 0;

    if (!query || !*query) {
        printf("usage: find <syscall_name|error|signal>\n");
        return;
    }
    if (strcmp(query, "error") == 0 || strcmp(query, "errors") == 0) {
        mode = 2;
    } else if (strcmp(query, "signal") == 0 ||
               strcmp(query, "signals") == 0) {
        mode = 1;
    } else if (etrace_syscall_number(r->trace->header.arch, query, &nr) != 0) {
        printf("unknown syscall: %s\n", query);
        return;
    }

    for (i = 0; i < r->count; i++) {
        const etrace_event_t *ev = event_at(r, i);
        if (!match_event(r, ev, nr, mode))
            continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            size_t *grown = realloc(matches, next * sizeof(*matches));
            if (!grown) {
                free(matches);
                printf("out of memory\n");
                return;
            }
            matches = grown;
            capacity = next;
        }
        matches[count++] = i;
    }

    if (count == 0) {
        printf("no events match '%s'\n", query);
        free(matches);
        return;
    }

    printf("%zu match(es) for '%s'", count, query);
    printf(": ");
    for (i = 0; i < count && i < 20; i++)
        printf("%s%zu", i ? ", " : "", matches[i]);
    if (count > 20)
        printf(", ...");
    printf("\n");

    for (i = 0; i < count; i++) {
        if (matches[i] >= r->current) {
            chosen = matches[i];
            have_choice = 1;
            break;
        }
    }
    if (!have_choice)
        chosen = matches[0];
    r->current = chosen;
    printf("jumped to step %zu\n", r->current);
    print_event_line(r, r->current);
    free(matches);
}

static void usage_replay(FILE *out)
{
    fprintf(out,
            "replay commands:\n"
            "  next, n [count]        step forward (default 1)\n"
            "  prev, p [count]        step backward (default 1)\n"
            "  goto <step>            jump to a 0-based step, or +N / -N\n"
            "  find <name|error|signal>  find events and jump to the next match\n"
            "  info                   registers, thread and syscall details\n"
            "  list, l [radius]       show events around the current step\n"
            "  threads                list the threads in the trace\n"
            "  header                 show the trace header\n"
            "  help, h                this help\n"
            "  quit, q                leave replay\n");
}

static int execute(replayer_t *r, char *line, int *quit)
{
    char *p = line;
    char *cmd;
    char *arg;
    long value;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (!*p || *p == '#')
        return 0;

    cmd = p;
    while (*p && !isspace((unsigned char)*p))
        p++;
    if (*p)
        *p++ = '\0';
    while (*p && isspace((unsigned char)*p))
        p++;
    arg = p;
    p = arg + strlen(arg);
    while (p > arg && isspace((unsigned char)p[-1]))
        *--p = '\0';

    if (strcmp(cmd, "n") == 0 || strcmp(cmd, "next") == 0) {
        value = 1;
        if (*arg && (parse_long(arg, &value) != 0 || value < 0)) {
            printf("usage: next [count]\n");
            return 0;
        }
        step_relative(r, value);
    } else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "prev") == 0 ||
               strcmp(cmd, "previous") == 0) {
        value = 1;
        if (*arg && (parse_long(arg, &value) != 0 || value < 0)) {
            printf("usage: prev [count]\n");
            return 0;
        }
        step_relative(r, -value);
    } else if (strcmp(cmd, "goto") == 0 || strcmp(cmd, "g") == 0) {
        step_goto(r, arg);
    } else if (strcmp(cmd, "find") == 0) {
        find_event(r, arg);
    } else if (strcmp(cmd, "info") == 0 || strcmp(cmd, "i") == 0) {
        print_info(r);
    } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "l") == 0) {
        value = 3;
        if (*arg && parse_long(arg, &value) != 0) {
            printf("usage: list [radius]\n");
            return 0;
        }
        print_list(r, value);
    } else if (strcmp(cmd, "threads") == 0) {
        print_threads(r);
    } else if (strcmp(cmd, "header") == 0) {
        print_header(r->trace, r->path);
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
        usage_replay(stdout);
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0 ||
               strcmp(cmd, "exit") == 0) {
        *quit = 1;
    } else if (strcmp(cmd, "?") == 0) {
        usage_replay(stdout);
    } else {
        printf("unknown command: %s (type 'help')\n", cmd);
    }
    return 0;
}

int etrace_replay(const char *path, size_t start_step,
                  const char *const *commands, size_t command_count)
{
    etrace_trace_t trace;
    replayer_t r;
    char err[256];
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    int quit = 0;
    size_t i;

    if (etrace_load_trace(path, &trace, err, sizeof(err)) != 0) {
        fprintf(stderr, "etrace: %s\n", err);
        return 1;
    }

    memset(&r, 0, sizeof(r));
    r.trace = &trace;
    r.path = path;
    r.count = trace.count;
    if (r.count > 0) {
        r.order = malloc(r.count * sizeof(*r.order));
        if (!r.order) {
            fprintf(stderr, "etrace: out of memory\n");
            etrace_close_trace(&trace);
            return 1;
        }
        for (i = 0; i < r.count; i++)
            r.order[i] = i;
        qsort_r(r.order, r.count, sizeof(*r.order), cmp_timeline, &r);
    }

    print_header(&trace, path);
    if (r.count > 0) {
        r.current = start_step < r.count ? start_step : r.count - 1;
        printf("\nCurrent event:\n");
        print_event_line(&r, r.current);
        printf("\n");
    } else {
        printf("\nThe trace has no events.\n");
    }

    if (command_count > 0) {
        for (i = 0; i < command_count && !quit; i++) {
            char *copy = strdup(commands[i]);
            if (!copy) {
                fprintf(stderr, "etrace: out of memory\n");
                break;
            }
            execute(&r, copy, &quit);
            free(copy);
        }
    } else {
        int interactive = isatty(STDIN_FILENO);
        while (!quit) {
            if (interactive) {
                printf("(etrace) ");
                fflush(stdout);
            }
            linelen = getline(&line, &linecap, stdin);
            if (linelen < 0)
                break;
            execute(&r, line, &quit);
        }
    }

    free(line);
    free(r.order);
    etrace_close_trace(&trace);
    return 0;
}

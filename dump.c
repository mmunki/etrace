#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "etrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long count_threads(const etrace_trace_t *trace)
{
    uint32_t *seen = NULL;
    size_t seen_count = 0, capacity = 0;
    size_t i;
    unsigned long threads = 0;

    for (i = 0; i < trace->count; i++) {
        int found = 0;
        size_t j;
        for (j = 0; j < seen_count; j++) {
            if (seen[j] == trace->events[i].tid) {
                found = 1;
                break;
            }
        }
        if (found)
            continue;
        if (seen_count == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            uint32_t *grown = realloc(seen, next * sizeof(*seen));
            if (!grown)
                break;
            seen = grown;
            capacity = next;
        }
        seen[seen_count++] = trace->events[i].tid;
        threads++;
    }
    free(seen);
    return threads;
}

int etrace_dump(const char *path, int summary_only, long limit, int errors_only)
{
    etrace_trace_t trace;
    char err[256];
    char call[1024];
    char ret[160];
    uint64_t duration;
    size_t shown = 0;
    size_t i;

    if (etrace_load_trace(path, &trace, err, sizeof(err)) != 0) {
        fprintf(stderr, "etrace: %s\n", err);
        return 1;
    }

    duration = trace.header.end_ns >= trace.header.start_ns
                   ? trace.header.end_ns - trace.header.start_ns
                   : 0;

    printf("File:     %s\n", path);
    printf("Format:   ETRACE v%u, %s (%s-endian)\n",
           (unsigned)trace.header.version,
           etrace_arch_name(trace.header.arch),
           "little");
    printf("Events:   %zu\n", trace.count);
    printf("Threads:  %lu\n", count_threads(&trace));
    printf("Start:    %llu ns (CLOCK_MONOTONIC)\n",
           (unsigned long long)trace.header.start_ns);
    printf("End:      %llu ns\n", (unsigned long long)trace.header.end_ns);
    printf("Duration: %llu.%09llu s\n",
           (unsigned long long)(duration / 1000000000ull),
           (unsigned long long)(duration % 1000000000ull));

    if (!summary_only && trace.count > 0) {
        printf("\n");
        for (i = 0; i < trace.count; i++) {
            const etrace_event_t *ev = &trace.events[i];

            if (errors_only &&
                !(ev->syscall_num == ETRACE_SIGNAL_SYSCALL || ev->ret_val < 0))
                continue;
            if (limit > 0 && shown >= (size_t)limit)
                break;

            etrace_format_call(call, sizeof(call), ev, trace.header.arch);
            etrace_format_ret(ret, sizeof(ret), ev, trace.header.arch);

            printf("[%6zu] ", i);
            etrace_print_relative_time(stdout, ev->timestamp_ns,
                                       trace.header.start_ns);
            printf(" pid=%u tid=%u %s%s\n", ev->pid, ev->tid, call, ret);
            shown++;
        }
        if (limit > 0 && shown < trace.count)
            printf("... %zu more event(s), use --limit 0 for all\n",
                   trace.count - shown);
    }

    etrace_close_trace(&trace);
    return 0;
}

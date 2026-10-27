#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "etrace.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t etrace_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

const char *etrace_arch_name(uint16_t arch)
{
    switch (arch) {
    case ETRACE_ARCH_X86_64: return "x86_64";
    case ETRACE_ARCH_AARCH64: return "aarch64";
    default: return "unknown";
    }
}

int etrace_arch_id(void)
{
#if defined(__x86_64__)
    return ETRACE_ARCH_X86_64;
#elif defined(__aarch64__)
    return ETRACE_ARCH_AARCH64;
#else
    return 0;
#endif
}

static void set_error(char *err, size_t errlen, const char *msg, const char *detail)
{
    if (!err || errlen == 0)
        return;
    if (detail)
        snprintf(err, errlen, "%s: %s", msg, detail);
    else
        snprintf(err, errlen, "%s", msg);
}

int etrace_load_trace(const char *path, etrace_trace_t *trace,
                      char *err, size_t errlen)
{
    struct stat st;
    etrace_header_t *header;

    memset(trace, 0, sizeof(*trace));
    trace->fd = -1;

    trace->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (trace->fd < 0) {
        set_error(err, errlen, "cannot open trace", strerror(errno));
        return -1;
    }
    if (fstat(trace->fd, &st) != 0) {
        set_error(err, errlen, "cannot stat trace", strerror(errno));
        goto fail;
    }
    if ((uint64_t)st.st_size < sizeof(etrace_header_t)) {
        set_error(err, errlen, "file is smaller than the etrace header", NULL);
        goto fail;
    }

    trace->map_len = (size_t)st.st_size;
    trace->map = mmap(NULL, trace->map_len, PROT_READ, MAP_PRIVATE,
                      trace->fd, 0);
    if (trace->map == MAP_FAILED) {
        trace->map = NULL;
        set_error(err, errlen, "cannot map trace", strerror(errno));
        goto fail;
    }

    header = (etrace_header_t *)trace->map;
    if (header->magic != ETRACE_MAGIC) {
        set_error(err, errlen, "not an etrace file (bad magic)", NULL);
        goto fail;
    }
    if (header->version != ETRACE_VERSION) {
        char detail[64];
        snprintf(detail, sizeof(detail), "version %u (expected %u)",
                 (unsigned)header->version, (unsigned)ETRACE_VERSION);
        set_error(err, errlen, "unsupported etrace version", detail);
        goto fail;
    }
    if (header->header_size != sizeof(etrace_header_t) ||
        header->event_size != sizeof(etrace_event_t)) {
        char detail[96];
        snprintf(detail, sizeof(detail), "header %u, event %u",
                 (unsigned)header->header_size, (unsigned)header->event_size);
        set_error(err, errlen, "unexpected record layout", detail);
        goto fail;
    }

    trace->header = *header;
    trace->events = (const etrace_event_t *)((const uint8_t *)trace->map +
                                             sizeof(etrace_header_t));

    if (header->event_count == 0 && trace->map_len > sizeof(etrace_header_t) &&
        (trace->map_len - sizeof(etrace_header_t)) % sizeof(etrace_event_t) == 0) {
        trace->count = (trace->map_len - sizeof(etrace_header_t)) /
                       sizeof(etrace_event_t);
    } else {
        uint64_t expected = sizeof(etrace_header_t) +
                            header->event_count * sizeof(etrace_event_t);
        if (expected > trace->map_len) {
            set_error(err, errlen, "trace is truncated", NULL);
            goto fail;
        }
        trace->count = (size_t)header->event_count;
    }

    return 0;

fail:
    etrace_close_trace(trace);
    return -1;
}

void etrace_close_trace(etrace_trace_t *trace)
{
    if (trace->map)
        munmap(trace->map, trace->map_len);
    if (trace->fd >= 0)
        close(trace->fd);
    memset(trace, 0, sizeof(*trace));
    trace->fd = -1;
}

int etrace_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

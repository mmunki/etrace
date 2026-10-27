#ifndef ETRACE_H
#define ETRACE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "etrace_format.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ETRACE_VERSION_STRING "0.1.0"

typedef struct {
    etrace_header_t header;
    const etrace_event_t *events;
    size_t count;
    int fd;
    void *map;
    size_t map_len;
} etrace_trace_t;

/* etrace.c */
uint64_t etrace_now_ns(void);
const char *etrace_arch_name(uint16_t arch);
int etrace_arch_id(void);
int etrace_load_trace(const char *path, etrace_trace_t *trace,
                      char *err, size_t errlen);
void etrace_close_trace(etrace_trace_t *trace);
int etrace_write_all(int fd, const void *buf, size_t len);

/* syscalls.c */
const char *etrace_syscall_name(uint16_t arch, uint32_t nr);
int etrace_syscall_number(uint16_t arch, const char *name, uint32_t *nr);

/* render.c */
const char *etrace_signal_name(int sig);
const char *etrace_signal_description(int sig);
void etrace_format_call(char *buf, size_t buflen,
                        const etrace_event_t *ev, uint16_t arch);
void etrace_format_ret(char *buf, size_t buflen,
                       const etrace_event_t *ev, uint16_t arch);
void etrace_print_relative_time(FILE *out, uint64_t ns, uint64_t base_ns);

/* recorder.c */
int etrace_record(int argc, char **argv);

/* replayer.c */
int etrace_replay(const char *path, size_t start_step,
                  const char *const *commands, size_t command_count);

/* dump.c */
int etrace_dump(const char *path, int summary_only, long limit, int errors_only);

#endif /* ETRACE_H */

/*
 * etrace_format.h -- on-disk format of an .etrace file.
 *
 * The format is little-endian and contains no pointers.  A trace recorded on
 * one machine can therefore be replayed or dumped on another architecture;
 * the architecture field only selects the syscall name table.
 *
 * Layout:
 *
 *   etrace_header_t
 *   etrace_event_t[event_count]
 *
 * All structures are packed, so their size is identical on x86_64 and
 * AArch64.  The header is written once at the start with event_count and
 * end_ns set to zero and is rewritten when recording finishes.
 */
#ifndef ETRACE_FORMAT_H
#define ETRACE_FORMAT_H

#include <stdint.h>

#define ETRACE_MAGIC 0x45545241u /* "ETRA" when read as big-endian ASCII */
#define ETRACE_VERSION 1u

#define ETRACE_ARCH_X86_64 1u
#define ETRACE_ARCH_AARCH64 2u

/* syscall_num marker for a signal-delivery event; args[0] is the signal. */
#define ETRACE_SIGNAL_SYSCALL 0xffffffffu

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* ETRACE_MAGIC */
    uint16_t version;     /* ETRACE_VERSION */
    uint16_t arch;        /* ETRACE_ARCH_* */
    uint32_t header_size; /* sizeof(etrace_header_t) */
    uint32_t event_size;  /* sizeof(etrace_event_t) */
    uint64_t start_ns;    /* CLOCK_MONOTONIC of the first event */
    uint64_t end_ns;      /* CLOCK_MONOTONIC when recording stopped */
    uint64_t event_count;
    uint8_t reserved[32]; /* zero, room for compatible extensions */
} etrace_header_t;

typedef struct __attribute__((packed)) {
    uint64_t timestamp_ns;
    uint32_t pid;  /* thread group id */
    uint32_t tid;  /* thread id */
    uint32_t syscall_num;
    int64_t ret_val; /* raw return, negative errno; zero on entry */
    uint64_t args[6];
} etrace_event_t;

_Static_assert(sizeof(etrace_header_t) == 72, "unexpected header layout");
_Static_assert(sizeof(etrace_event_t) == 76, "unexpected event layout");

#endif /* ETRACE_FORMAT_H */

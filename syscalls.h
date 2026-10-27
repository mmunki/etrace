#ifndef ETRACE_SYSCALLS_H
#define ETRACE_SYSCALLS_H

#include "etrace_format.h"
#include "syscall_names.h"

static inline size_t etrace_syscall_table_size(uint16_t arch)
{
    if (arch == ETRACE_ARCH_X86_64)
        return sizeof(etrace_syscalls_x86_64) / sizeof(etrace_syscalls_x86_64[0]);
    if (arch == ETRACE_ARCH_AARCH64)
        return sizeof(etrace_syscalls_aarch64) / sizeof(etrace_syscalls_aarch64[0]);
    return 0;
}

static inline const etrace_syscall_name_t *etrace_syscall_table(uint16_t arch)
{
    if (arch == ETRACE_ARCH_X86_64)
        return etrace_syscalls_x86_64;
    if (arch == ETRACE_ARCH_AARCH64)
        return etrace_syscalls_aarch64;
    return NULL;
}

static inline size_t etrace_alias_table_size(uint16_t arch)
{
    if (arch == ETRACE_ARCH_X86_64)
        return sizeof(etrace_syscall_aliases_x86_64) / sizeof(etrace_syscall_aliases_x86_64[0]);
    if (arch == ETRACE_ARCH_AARCH64)
        return sizeof(etrace_syscall_aliases_aarch64) / sizeof(etrace_syscall_aliases_aarch64[0]);
    return 0;
}

static inline const etrace_syscall_alias_t *etrace_alias_table(uint16_t arch)
{
    if (arch == ETRACE_ARCH_X86_64)
        return etrace_syscall_aliases_x86_64;
    if (arch == ETRACE_ARCH_AARCH64)
        return etrace_syscall_aliases_aarch64;
    return NULL;
}

#endif /* ETRACE_SYSCALLS_H */

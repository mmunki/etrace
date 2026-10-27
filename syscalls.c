#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "etrace.h"
#include "syscalls.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *etrace_syscall_name(uint16_t arch, uint32_t nr)
{
    const etrace_syscall_name_t *table = etrace_syscall_table(arch);
    size_t lo = 0, hi = etrace_syscall_table_size(arch);

    if (!table)
        return NULL;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (table[mid].nr == nr)
            return table[mid].name;
        if (table[mid].nr < nr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static void normalize_name(const char *input, char *out, size_t outlen)
{
    size_t i;
    for (i = 0; input[i] && i + 1 < outlen; i++)
        out[i] = (char)tolower((unsigned char)input[i]);
    out[i] = '\0';

    for (;;) {
        const char *prefix = NULL;
        if (strncmp(out, "__x64_sys_", 10) == 0)
            prefix = out + 10;
        else if (strncmp(out, "__arm64_sys_", 12) == 0)
            prefix = out + 12;
        else if (strncmp(out, "__nr_", 5) == 0)
            prefix = out + 5;
        else if (strncmp(out, "sys_", 4) == 0)
            prefix = out + 4;
        if (!prefix)
            break;
        memmove(out, prefix, strlen(prefix) + 1);
    }
}

int etrace_syscall_number(uint16_t arch, const char *name, uint32_t *nr)
{
    const etrace_syscall_name_t *table = etrace_syscall_table(arch);
    const etrace_syscall_alias_t *aliases = etrace_alias_table(arch);
    size_t count = etrace_syscall_table_size(arch);
    char normalized[128];
    char *end = NULL;
    unsigned long numeric;
    size_t i;

    if (!table || !name || !nr)
        return -1;

    if (name[0] && isdigit((unsigned char)name[0])) {
        numeric = strtoul(name, &end, 0);
        if (end && *end == '\0') {
            *nr = (uint32_t)numeric;
            return 0;
        }
    }

    normalize_name(name, normalized, sizeof(normalized));
    for (i = 0; i < count; i++) {
        if (strcmp(table[i].name, normalized) == 0) {
            *nr = table[i].nr;
            return 0;
        }
    }
    for (i = 0; i < etrace_alias_table_size(arch); i++) {
        if (strcmp(aliases[i].alias, normalized) == 0) {
            *nr = aliases[i].nr;
            return 0;
        }
    }
    return -1;
}

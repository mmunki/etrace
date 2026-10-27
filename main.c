#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "etrace.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out,
            "etrace %s -- Linux syscall recorder and time-travel replay\n"
            "\n"
            "usage:\n"
            "  etrace record [options] [--] <command> [args...]\n"
            "  etrace replay [options] <file.etrace>\n"
            "  etrace dump [options] <file.etrace>\n"
            "  etrace version\n"
            "  etrace help\n"
            "\n"
            "record options:\n"
            "  -o, --output FILE   write the trace to FILE (default: trace.etrace)\n"
            "      --no-follow     do not follow clone/fork/vfork children\n"
            "      --quiet         do not print a summary\n"
            "\n"
            "replay options:\n"
            "  -c, --command CMD   execute CMD and exit (repeatable)\n"
            "      --step N        start at timeline step N (default: 0)\n"
            "\n"
            "dump options:\n"
            "      --summary       print only the header\n"
            "      --limit N       print at most N events (0 = all)\n"
            "      --errors        print only failed syscalls and signals\n"
            "\n"
            "examples:\n"
            "  etrace record -o app.etrace -- ./my_app\n"
            "  etrace replay app.etrace\n"
            "  etrace dump app.etrace --errors\n",
            ETRACE_VERSION_STRING);
}

static int parse_size(const char *s, size_t *out)
{
    char *end = NULL;
    unsigned long long value;
    if (!s || !*s)
        return -1;
    errno = 0;
    value = strtoull(s, &end, 0);
    if (errno || !end || *end != '\0')
        return -1;
    *out = (size_t)value;
    return 0;
}

static int command_replay(int argc, char **argv)
{
    const char *path = NULL;
    const char **commands = NULL;
    size_t command_count = 0, command_cap = 0;
    size_t start_step = 0;
    int i;

    for (i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            free(commands);
            return 0;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--command") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "etrace: %s needs a value\n", arg);
                free(commands);
                return 2;
            }
            if (command_count == command_cap) {
                size_t next = command_cap ? command_cap * 2 : 8;
                const char **grown = realloc(commands, next * sizeof(*commands));
                if (!grown) {
                    fprintf(stderr, "etrace: out of memory\n");
                    free(commands);
                    return 1;
                }
                commands = grown;
                command_cap = next;
            }
            commands[command_count++] = argv[++i];
            continue;
        }
        if (strncmp(arg, "--command=", 10) == 0) {
            if (command_count == command_cap) {
                size_t next = command_cap ? command_cap * 2 : 8;
                const char **grown = realloc(commands, next * sizeof(*commands));
                if (!grown) {
                    fprintf(stderr, "etrace: out of memory\n");
                    free(commands);
                    return 1;
                }
                commands = grown;
                command_cap = next;
            }
            commands[command_count++] = arg + 10;
            continue;
        }
        if (strcmp(arg, "--step") == 0) {
            if (i + 1 >= argc || parse_size(argv[i + 1], &start_step) != 0) {
                fprintf(stderr, "etrace: --step needs a non-negative integer\n");
                free(commands);
                return 2;
            }
            i++;
            continue;
        }
        if (strncmp(arg, "--step=", 7) == 0) {
            if (parse_size(arg + 7, &start_step) != 0) {
                fprintf(stderr, "etrace: --step needs a non-negative integer\n");
                free(commands);
                return 2;
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "etrace: unknown replay option: %s\n", arg);
            free(commands);
            return 2;
        }
        if (path) {
            fprintf(stderr, "etrace: replay takes one trace file\n");
            free(commands);
            return 2;
        }
        path = arg;
    }

    if (!path) {
        fprintf(stderr, "etrace: replay needs a trace file\n");
        usage(stderr);
        free(commands);
        return 2;
    }

    i = etrace_replay(path, start_step, commands, command_count);
    free(commands);
    return i;
}

static int command_dump(int argc, char **argv)
{
    const char *path = NULL;
    long limit = -1;
    int summary = 0;
    int errors = 0;
    int i;

    for (i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(arg, "--summary") == 0) {
            summary = 1;
            continue;
        }
        if (strcmp(arg, "--errors") == 0) {
            errors = 1;
            continue;
        }
        if (strcmp(arg, "--limit") == 0) {
            char *end = NULL;
            if (i + 1 >= argc) {
                fprintf(stderr, "etrace: --limit needs a value\n");
                return 2;
            }
            errno = 0;
            limit = strtol(argv[++i], &end, 0);
            if (errno || !end || *end != '\0' || limit < 0) {
                fprintf(stderr, "etrace: --limit needs a non-negative integer\n");
                return 2;
            }
            continue;
        }
        if (strncmp(arg, "--limit=", 8) == 0) {
            char *end = NULL;
            errno = 0;
            limit = strtol(arg + 8, &end, 0);
            if (errno || !end || *end != '\0' || limit < 0) {
                fprintf(stderr, "etrace: --limit needs a non-negative integer\n");
                return 2;
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "etrace: unknown dump option: %s\n", arg);
            return 2;
        }
        if (path) {
            fprintf(stderr, "etrace: dump takes one trace file\n");
            return 2;
        }
        path = arg;
    }

    if (!path) {
        fprintf(stderr, "etrace: dump needs a trace file\n");
        usage(stderr);
        return 2;
    }
    return etrace_dump(path, summary, limit, errors);
}

int main(int argc, char **argv)
{
    const char *cmd;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "-V") == 0 ||
        strcmp(cmd, "--version") == 0) {
        printf("etrace %s\n", ETRACE_VERSION_STRING);
        return 0;
    }
    if (strcmp(cmd, "record") == 0)
        return etrace_record(argc - 2, argv + 2);
    if (strcmp(cmd, "replay") == 0)
        return command_replay(argc - 2, argv + 2);
    if (strcmp(cmd, "dump") == 0)
        return command_dump(argc - 2, argv + 2);

    fprintf(stderr, "etrace: unknown command: %s\n\n", cmd);
    usage(stderr);
    return 2;
}

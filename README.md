# etrace

`etrace` records Linux system calls into a compact binary `.etrace` file and
replays them in an interactive stepper that can move **forward and backward**
through the timeline.  It is a single, dependency-free C program using the
native `ptrace` interface, with no instrumentation and no daemon in the target:

```
etrace record -o app.etrace -- ./my_app
etrace replay app.etrace
etrace dump app.etrace
```

## What it does

- Attaches to a command at `PTRACE_TRACEME`/`execve` time, before `main()`.
- Follows syscall entry and exit for every thread (`PTRACE_O_TRACECLONE`,
  `PTRACE_O_TRACEFORK`, `PTRACE_O_TRACEVFORK`).
- Records one fixed-size event per completed syscall:
  - `CLOCK_MONOTONIC` timestamp captured at syscall **entry**,
  - pid and tid,
  - syscall number and the six ABI register arguments,
  - the raw return value / negative errno.
- Records a signal-delivery event for fatal signals such as `SIGSEGV`, so the
  crash is the last frame of the timeline.
- Writes with a 1 MiB buffered stream; the target is never ptrace-detached
  during recording.

The binary format is little-endian, packed, and contains no pointers, so a
trace recorded on x86_64 can be dumped or replayed on AArch64 and the other
way around.  The architecture ID in the header selects the syscall name table.

The overhead is one ptrace stop per syscall entry and one per exit.  There is
no target instrumentation, and recording does not keep the whole trace in
memory.

## Build

Requirements: a C11 compiler, GNU make, Linux kernel 5.3 or newer for
`PTRACE_GET_SYSCALL_INFO` (older kernels use the register fallback).

```bash
make                  # dynamically linked ./etrace and ./test_crash
make test             # build and run the full test suite
make release          # static ./dist/etrace-$(uname -m)
```

Static build for another ABI:

```bash
sudo apt-get install gcc-aarch64-linux-gnu
make release CC=aarch64-linux-gnu-gcc ARCH_NAME=aarch64
```

CMake works too:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Install:

```bash
make install PREFIX=/usr/local
```

## Usage

### record

```
etrace record [options] [--] <command> [args...]

  -o, --output FILE   write the trace to FILE (default: trace.etrace)
      --no-follow     do not follow clone/fork/vfork children
      --quiet         do not print a summary
```

Example:

```console
$ etrace record -o crash.etrace -- ./test_crash
etrace: recorded 59 events from 2 thread(s) in 0.098820898 s (x86_64) -> crash.etrace
etrace: target terminated by signal 11 (SIGSEGV): Segmentation fault
$ echo $?
139
```

The recorder returns the target's exit status, or `128 + signal` when the
target dies from a signal.

### replay

```
etrace replay [options] <file.etrace>

  -c, --command CMD   execute CMD and exit (repeatable)
      --step N        start at timeline step N (default: 0)
```

When stdin is a terminal the stepper prints an `(etrace)` prompt.  Commands can
also be piped or passed with `-c`, which is what the tests do.

| Command | Meaning |
| --- | --- |
| `next`, `n [count]` | step forward, default 1 |
| `prev`, `p [count]` | step **backward**, default 1 |
| `goto <step>` | jump to an absolute 0-based timeline step |
| `goto +N`, `goto -N` | jump relative to the current step |
| `find <name>` | search a syscall, then jump to the next match |
| `find error` | find failed syscalls |
| `find signal` | find signal-delivery events |
| `info` | registers, pid/tid, syscall translation, thread statistics |
| `list [radius]` | show events around the current step |
| `threads` | list every thread in the trace |
| `header` | show the trace header |
| `quit`, `q` | leave replay |

Example: inspect the state before the crash:

```console
$ etrace replay crash.etrace
...
(etrace) find signal
(etrace) info
Step     58 of 58 (record #58)
Time     +0.003090132 s   absolute 611334143106 ns
Thread   pid=4197 tid=4197 (48 events, steps 0..58)
Event    signal #11
Args     rdi=0xb rsi=0x0 rdx=0x0 r10=0x0 r8=0x0 r9=0x0
Return   -11
Call     signal SIGSEGV (11): Segmentation fault
(etrace) prev 2
(etrace) info
...
```

`next`, `prev`, and `goto` move over a timeline sorted by timestamp, so the
steps follow wall-clock order even when several threads interleave.  `info`
also reports the original record number in the file.

### dump

```
etrace dump [options] <file.etrace>

      --summary       print only the header
      --limit N       print at most N events (0 = all)
      --errors        print only failed syscalls and signals
```

```console
$ etrace dump crash.etrace --errors
File:     crash.etrace
Format:   ETRACE v1, x86_64 (little-endian)
Events:   59
...
[    47] +0.002523868 pid=4197 tid=4198 openat(dirfd=AT_FDCWD, path=0x557faf74d004, flags=O_RDONLY|O_LARGEFILE|O_CLOEXEC, mode=00) = -2 (No such file or directory)
[    58] +0.003090132 pid=4197 tid=4197 signal SIGSEGV (11): Segmentation fault
```

The textual form translates the common syscalls (`openat`, `read`, `write`,
`mmap`, `clone`, `futex`, `lseek`, `statx`, socket calls, signal syscalls, ...)
and decodes `open`, `mmap`, `mprotect`, `clone` and `futex` flags.  Arguments
are the raw register values; string contents are intentionally not copied into
the fixed-size event.

## Binary format

`etrace_format.h` is the complete definition:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;       /* 0x45545241 */
    uint16_t version;     /* 1 */
    uint16_t arch;        /* 1 = x86_64, 2 = aarch64 */
    uint32_t header_size; /* 72 */
    uint32_t event_size;  /* 76 */
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t event_count;
    uint8_t  reserved[32];
} etrace_header_t;

typedef struct __attribute__((packed)) {
    uint64_t timestamp_ns;
    uint32_t pid;
    uint32_t tid;
    uint32_t syscall_num;
    int64_t  ret_val;
    uint64_t args[6];
} etrace_event_t;
```

A file is exactly one header followed by `event_count` events.  The header is
written as a placeholder first and rewritten when recording stops.  A signal
event uses `syscall_num = 0xffffffff` and puts the signal number in `args[0]`;
`ret_val` is `-signal`.

Register mapping:

| ABI | syscall number | arguments |
| --- | --- | --- |
| x86_64 | `orig_rax` | `rdi, rsi, rdx, r10, r8, r9` |
| AArch64 | `x8` | `x0, x1, x2, x3, x4, x5` |

## Tests

`test_crash.c` is a small fixture that does file I/O, starts and joins a worker
thread, performs one failing `openat`, and then dereferences a null pointer.
`tests/run_tests.sh` records it and checks:

- the magic/version/size/count of the binary header,
- that `openat`, `write`, `read`, `clone`, the `ENOENT` error and `SIGSEGV`
  appear in `dump`,
- `dump --summary`, `--limit` and `--errors`,
- interactive and `-c` replay, including `find sys_openat`, `find error`,
  `find signal`, `threads`, `header`,
- `goto`, `prev` and clamping at the start of the timeline.

CI runs the same suite twice on native x86_64 and native AArch64 GitHub
runners: once with the normal build and once with the statically linked release
binary.

## Limitations

- Only x86_64 and AArch64 are implemented; other architectures fail at compile
  time.
- `ptrace` permits one tracer per process.  A tracee that refuses ptrace
  (`/proc/sys/kernel/yama/ptrace_scope`, seccomp) cannot be recorded.
- String arguments are not stored; `path=` and `buf=` show the pointer value.
  The fixed event layout is deliberate and keeps replay zero-copy.
- The recorder follows clone/fork/vfork children by default; this includes
  short-lived helper processes, which can increase event volume.
- The format is little-endian.  Big-endian hosts are not supported.

## License

MIT.  See `LICENSE`.

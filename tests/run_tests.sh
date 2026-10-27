#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${ETRACE_BIN:-./etrace}"
CRASH="${TEST_CRASH:-./test_crash}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/etrace-test.XXXXXX")"

cleanup() {
    rm -rf "$WORK"
    rm -f etrace-main.tmp etrace-thread.tmp
}
trap cleanup EXIT

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'ok - %s\n' "$*"; }

[ -x "$BIN" ] || fail "etrace binary not found: $BIN"
[ -x "$CRASH" ] || fail "test fixture not found: $CRASH"

"$BIN" version | grep -Eq '^etrace [0-9]+\.[0-9]+\.[0-9]+$' \
    || fail "unexpected version output"
pass "version"

trace="$WORK/crash.etrace"
set +e
"$BIN" record -o "$trace" -- "$CRASH" >"$WORK/fixture.out" 2>"$WORK/record.err"
rc=$?
set -e

[ -s "$trace" ] || { cat "$WORK/record.err" >&2; fail "record produced no trace"; }
[ "$rc" -ne 0 ] || fail "the fixture was supposed to crash but etrace returned success"
grep -q 'recorded .* event' "$WORK/record.err" || fail "record summary missing"
grep -q 'terminated by signal 11' "$WORK/record.err" || fail "SIGSEGV was not reported"
pass "record captured the crash (exit $rc)"

python3 - "$trace" <<'PY'
import struct
import sys

data = open(sys.argv[1], "rb").read()
magic, version, arch, header_size, event_size, start, end, count = \
    struct.unpack("<IHHIIQQQ", data[:40])
assert magic == 0x45545241, hex(magic)
assert version == 1, version
assert arch in (1, 2), arch
assert header_size == 72, header_size
assert event_size == 76, event_size
assert count > 0, count
assert start > 0 and end >= start
assert len(data) == header_size + count * event_size, (len(data), count)
print("format ok: arch=%d events=%d size=%d" % (arch, count, len(data)))
PY
pass "binary format and header are valid"

"$BIN" dump "$trace" --summary >"$WORK/summary.txt"
grep -q 'Format:.*ETRACE v1' "$WORK/summary.txt" || fail "summary format line"
grep -Eq 'Events:   [1-9]' "$WORK/summary.txt" || fail "summary event count"
grep -Eq 'Threads:  [2-9]' "$WORK/summary.txt" || fail "worker thread was not followed"
pass "dump --summary"

"$BIN" dump "$trace" >"$WORK/dump.txt"
for name in openat write read clone; do
    grep -q "$name" "$WORK/dump.txt" || fail "dump is missing $name"
done
grep -q 'signal SIGSEGV' "$WORK/dump.txt" || fail "dump is missing the SIGSEGV event"
pass "dump contains file I/O, clone, and the SIGSEGV event"

"$BIN" dump "$trace" --errors >"$WORK/errors.txt"
grep -q 'signal SIGSEGV' "$WORK/errors.txt" || fail "dump --errors missed the signal"
grep -q -- '= -2 (' "$WORK/errors.txt" || fail "dump --errors missed the ENOENT"
pass "dump --errors"

"$BIN" dump "$trace" --limit 3 >"$WORK/limit.txt"
lines="$(grep -c '^\[' "$WORK/limit.txt" || true)"
[ "$lines" -eq 3 ] || fail "dump --limit printed $lines event lines, expected 3"
pass "dump --limit"

{
    echo info
    echo 'next 2'
    echo info
    echo prev
    echo 'goto 0'
    echo info
    echo 'find sys_openat'
    echo info
    echo 'find error'
    echo 'find signal'
    echo threads
    echo header
    echo quit
} | "$BIN" replay "$trace" >"$WORK/replay.txt"

grep -q '^Step' "$WORK/replay.txt" || fail "replay info has no step line"
grep -q '^Syscall' "$WORK/replay.txt" || fail "replay info has no syscall line"
grep -q '^Args ' "$WORK/replay.txt" || fail "replay info has no register line"
grep -q '^Return' "$WORK/replay.txt" || fail "replay info has no return line"
grep -q "match(es) for 'sys_openat'" "$WORK/replay.txt" || fail "find sys_openat failed"
grep -q "match(es) for 'error'" "$WORK/replay.txt" || fail "find error failed"
grep -q "match(es) for 'signal'" "$WORK/replay.txt" || fail "find signal failed"
grep -q 'thread(s)' "$WORK/replay.txt" || fail "threads command failed"
pass "interactive replay commands"

"$BIN" replay "$trace" --step 3 \
    -c info -c prev -c info -c 'goto 0' -c info -c prev -c info -c quit \
    >"$WORK/time-travel.txt"
grep -q 'Step     3 of' "$WORK/time-travel.txt" || fail "--step did not start at 3"
grep -q 'Step     2 of' "$WORK/time-travel.txt" || fail "prev did not step backward"
grep -q 'Step     0 of' "$WORK/time-travel.txt" || fail "goto 0 did not jump to the start"
pass "time travel (goto/prev/clamping)"

printf 'ALL TESTS PASSED (%s)\n' "$BIN"

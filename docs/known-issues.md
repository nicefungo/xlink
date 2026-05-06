# Known Issues

## 1. xlink_read() silently ignores timeout_ms on backends without .read vtable

**Status**: ✅ **5 backends fixed** (TCP, Pipe, Serial, UDP, File). Only SHM remains
— `shm_ipc` doesn't expose a pollable file descriptor, so `poll()` can't be used.

**Fix** (`abf9880` TCP, `ba66ba7` Pipe/Serial/UDP, `7ce9b15` File): Each polls
`ch->fd` with `poll(fd, POLLIN, timeout_ms)` then delegates to `.recv()`. For
regular files (File backend), `poll()` always returns immediately on Linux
(files are always "ready"), so timeout is effectively ignored — same behavior
as the old NULL fallback.

## 2. SHM atexit cleanup has a fixed 64-entry cap

**Status**: ✅ Fixed in `0d13a4d`

**Fix**: `MAX_CLEANUP` bumped from 64 to 256 (2KB BSS). Processes creating up
to 256 SHM segments with `XLINK_CREATE` are fully tracked for atexit cleanup.
Still have the cap — beyond 256 leaks, but that requires extremely aggressive
SHM usage.

## 3. TCP NONBLOCK + oversized message discard on pipe backend (same code in xlink.c)

**Status**: By design

**Affected code**: `src/xlink.c` — `frame_recv()` discard path

**Description**: When `frame_recv` discards an oversized message in the generic
framer, the error path during discard uses `n <= 0` to detect EOF/error. If
`read_exact` returns 0 in the discard loop (unlikely, since we always request
>0 bytes), the error message says "partial discard" rather than "EOF".

This is extremely unlikely in practice — the discard loop always calls
`read_exact` with `to_read > 0`, so a return of 0 would mean EOF mid-discard.

## 4. test_tcp_overflow_client depends on success of initial connection

**Status**: Minor (test fragility)

**Affected test**: `tests/test_tcp_overflow_client.c`

**Description**: The test opens a raw TCP server socket that must accept
connections before the xlink client connects. Port reuse is enabled, but
if another test happens to use the same port (`19897`) simultaneously, the
test can fail.

## 5. test_frame_overflow uses hardcoded port 19992

**Status**: Minor (test fragility)

**Affected test**: `tests/test_frame_overflow.c`

**Description**: All TCP tests use hardcoded ports. Running tests in parallel
with overlapping port ranges would cause failures. Currently the test runner
is serial (`make test`), so this isn't a problem.

## 6. Serial backend defaults to 9600 for unknown baud rates

**Status**: By design

**Affected code**: `src/serial_backend.c` — `baud_to_speed()`

**Description**: If a baud rate outside the known set is specified (e.g. 500),
it silently defaults to 9600. This could cause communication mismatches with
the remote device. Always specify a known baud rate (4800, 9600, 19200, 38400,
57600, 115200, 230400).

## 7. Makefile hardcodes test targets — new tests manually added

**Status**: ✅ Fixed in `99f560d`

**Fix**: Replaced hardcoded test compilation lines with a wildcard pattern:
```makefile
TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(patsubst tests/%.c, bin/tests/%, $(TEST_SRCS))
```
New `test_*.c` files are auto-discovered. No Makefile edits needed.

## 8. UDP NONBLOCK recv: recvfrom() with EAGAIN returns -1 without errbuf message

**Status**: ✅ Fixed in `4e04816`

**Fix**: `udp_backend_recv()` now sets errbuf to `"udp: no data"` instead of
`"udp recvfrom: Resource temporarily unavailable"` when `recvfrom()` returns
`EAGAIN` or `EWOULDBLOCK`.

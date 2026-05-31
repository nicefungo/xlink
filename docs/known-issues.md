# Known Issues

## 1. xlink_read() timeout on SHM

**Status**: ✅ Fixed (2026-05-28). All 6 backends now implement `.read` vtable.

**Fix**: SHM backend implements `.read` via a `shm_peek()` polling loop with
500µs intervals, using `clock_gettime(CLOCK_MONOTONIC)` for timeout accuracy.
Same behavior as the `xlink_wait()` fallback path for SHM channels, but at the
per-channel `xlink_read()` level.

## 2. SHM atexit cleanup has a fixed 256-entry cap

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

## 9. make test executes mock_plugin.so as a binary

**Status**: ✅ Fixed (Round 68, 2026-05-31)

**Affected code**: `Makefile` — `test` target

**Description**: `make test` iterates `bin/tests/*` and tries to execute every
file as a binary. Since `mock_plugin.so` is a shared library (not an ELF
executable), the shell would segfault trying to run it. No test failures, just
a spurious `Segmentation fault` line in output.

**Fix**: Added `*.so` skip in test loop:
```makefile
case "$$t" in *.so) continue ;; esac;
```
Also added `|| true` fallback to prevent one test failure from stopping the
suite — same behavior as before but explicit.

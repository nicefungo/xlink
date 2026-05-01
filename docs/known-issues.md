# Known Issues

## 1. xlink_read() silently ignores timeout_ms on backends without .read vtable

**Status**: Minor (design limitation)

**Affected backends**: SHM, UDP, File (`.read = NULL`)

**Description**: `xlink_read()` accepts a `timeout_ms` parameter, but on backends
where `.read` is not defined in the vtable, it falls back to:
```c
if (ch->backend->read)
    return ch->backend->read(ch, buf, len, timeout_ms);
/* fallback: ignores timeout_ms */
size_t n = len;
if (ch->backend->recv(ch, buf, &n) == 0)
    return (int)n;
return -1;
```
The `timeout_ms` value is silently dropped. This means `xlink_read()` with a
short timeout on SHM/UDP/File backends blocks indefinitely.

**Workaround**: Only use `xlink_read()` with timeout on backends where `.read` is
defined (currently none — this is for future expansion). Use `xlink_recv()`
with `xlink_wait()` for timed receive on those backends.

## 2. SHM atexit cleanup has a fixed 64-entry cap

**Status**: Minor (potential leak in aggressive multi-SHM use)

**Affected code**: `src/xlink.c` — `shm_cleanup_all()` / `MAX_CLEANUP`

**Description**: The SHM cleanup registry limits cleanup to 64 names. If a
process creates >64 SHM segments with `XLINK_CREATE`, the extras are silently
dropped and will leak if `atexit` is the only cleanup path.

**Workaround**: Limit per-process SHM usage to ≤64 named segments. Or use
explicit `shm_destroy()` for segments beyond the 64th.

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

**Status**: Minor (maintenance overhead)

**Description**: Adding a new test requires editing the Makefile. A wildcard
scan (`bin/tests/*.c`?) would be more maintainable but was deferred for
explicit control over build dependencies.

## 8. UDP NONBLOCK recv: recvfrom() with EAGAIN returns -1 without errbuf message

**Status**: Minor

**Affected code**: `src/udp_backend.c` — `udp_backend_recv()`

**Description**: When `XLINK_NONBLOCK` is set on a UDP receiver and no datagram
is available, `recvfrom()` returns -1 with `errno = EAGAIN`. The error buffer
message says "udp recvfrom: Resource temporarily unavailable", which could be
confusing. An explicit "no data" message would be clearer.

This is a cosmetic issue — the API contract (return -1 when no data) is correct.

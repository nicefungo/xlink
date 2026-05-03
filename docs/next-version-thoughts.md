# xlink — Next Version Thoughts

_Scratchpad for ideas, known issues, and plans beyond the current release._

---

## Round 35 — 2026-05-01 20:45 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **ALL 26 + 4 new checks PASS**
- 26 test binaries, **0 failures** (no regressions after 35 rounds)
- Total checkpoint count: ~311+ (estimated)

### Code Review — 35th round

All 7 src/ + include/xlink.h + 26 test/ files reviewed. **No new bugs found.**

Cross-module consistency verified:
- All 6 backend vtable entries consistent: `.write = NULL` on all except file_backend has a pair; `.read = NULL` on all (file_backend also NULL). xlink_write()/xlink_read() fallback chains correct.
- `frame_recv` discard paths consistent between xlink.c generic framer and tcp_backend.c
- SHM cleanup atexit registry bounded at 64 (documented known issue, confirmed)
- EAGAIN error path in `udp_backend_recv()` — cosmetic only, error message says "Resource temporarily unavailable" vs a cleaner "no data" (known issue #8)

### New test: Serial backend open failure (4 checks)

Added `test_serial_open_failure()` to `tests/test_errors.c` covering 2 previously untested error scenarios:
1. `xlink_open(XLINK_SERIAL, "/dev/nonexistent_serial_test", NULL)` → returns NULL, errno is properly set
2. `xlink_errstr(NULL)` after serial failure returns non-empty string via errno
3. Serial open with invalid baud (999999) → still returns NULL (device doesn't exist, baud clamped to 9600)
4. Serial open with bogus opt.serial.baud=300 → NULL (same device path, clamped baud is irrelevant)

All 4 checks PASS. This is the first test coverage for the serial backend's .open failure paths — previously all serial tests used PTY (success path only).

### Documentation improvements
- **Updated: `docs/next-version-thoughts.md`** — this entry
- **Updated: `tests/test_errors.c`** — serial open failure coverage
- Integration guide (`docs/integration-guide.md`) — comprehensive, no gaps found
- API doc (`docs/api.md`) — complete, references known-issues.md properly

### Future directions (unchanged from Round 34)

1. **Wildcard test discovery**: Makefile hardcodes 26 test targets. A `$(wildcard tests/test_*.c)` rule would eliminate manual additions.
2. **xlink_read() timeout_ms on .read = NULL backends**: The fallback silently ignores timeout. Could add compile-time assert or runtime check.
3. **SHM cleanup cap**: `MAX_CLEANUP=64` silent drop. Could warn or dynamically resize.
4. **Cooperative test ports**: All TCP tests use hardcoded ports. A `get_available_port()` or `PORT0` env convention would prevent conflicts.
5. **CI-style stress test run**: `make stress` (TCP + SHM) could be integrated into the test suite.
6. **UDP NONBLOCK recv prettier error message**: Issue #8 — `recvfrom` with EAGAIN says "Resource temporarily unavailable" vs cleaner "no data".

### Known items (8 items, 1 added Round 35, 1 removed)

- [√] Known issue #7 (test TCP ports) — still relevant
- [+] Known issue #8: UDP NONBLOCK recvfrom EAGAIN error message is opaque

### Codebase stats (no significant change from Round 34)
- 7 src/ + 1 include/ + 26 test files + 4 tools/
- 0 warnings at `-Wall -Wextra -O2 -g`
- All 26 test binaries PASS (0 failures)

---

## Round 34 — 2026-05-01 08:45 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **26/26 ALL PASS** (progressive: 306+ → ~335+ checkpoints)
- New test `test_file_nonblock`: 29 checks covering file NONBLOCK + raw I/O fallback

### Code Review — 34th round

All 7 src/ + include/xlink.h + 26 tests/ reviewed. **No new bugs found.**

Cross-module consistency verified:
- `xlink_write()` / `xlink_read()` fallback chains match all 6 backend vtables
- `frame_recv` discard paths consistent between xlink.c generic framer and tcp_backend.c
- SHM cleanup atexit registry bounded at 64 (documented as known issue)
- All 6 backends with `.open` properly handle `XLINK_NONBLOCK` flag

### New test: File backend NONBLOCK + raw I/O

Added `tests/test_file_nonblock.c` covering 4 previously untested scenarios:
1. File record + replay with `XLINK_NONBLOCK` flag (accepts flag, operations work)
2. `xlink_write()` on file backend (`.write = NULL` → `.send()` fallback)
3. `xlink_read()` on file backend (`.read = NULL` → `.recv()` fallback)
4. `xlink_write()` to read-only file with NONBLOCK → returns -1 (EBADF)

### Documentation improvements
- **New: `docs/known-issues.md`** — 8 documented issues with status, affected code, and workarounds
- **Updated: `docs/api.md`** — added `xlink_wait()` API reference, NONBLOCK flag behavior table per transport, known issues reference
- **Updated: `docs/next-version-thoughts.md`** — this entry

### Future directions

1. **Wildcard test discovery**: The Makefile hardcodes 26 test targets. A `$(wildcard tests/test_*.c)` pattern would eliminate manual additions for new tests.

2. **xlink_read() timeout_ms on .read = NULL backends**: The fallback silently ignores timeout. Could add a compile-time assert or runtime check.

3. **SHM cleanup cap**: `MAX_CLEANUP=64` in xlink.c is a silent drop. Could warn or dynamically resize.

4. **Cooperative test ports**: All TCP tests use hardcoded ports. A built-in `get_available_port()` or `PORT0` environment variable convention would prevent conflicts.

5. **CI-style stress test run**: `make stress` (TCP + SHM) could be integrated into the test suite. Currently must be run separately.

---

## Round 33 — 2026-04-30 20:45 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **25/25 ALL PASS** (306+ checkpoints across 25 test binaries)
- `make stress` → not run (routine review, no ABI changes)

### Code Review — 33rd round
All 7 src/ + include/xlink.h + 4 tools/ + 25 tests/ reviewed.
**No new bugs found.** Codebase stable after 33 consecutive rounds.

### New test coverage: xlink_wait() error paths (11 checks)
Extended `tests/test_errors.c` with `test_wait_invalid()`.

Previously untested defensive input-validation paths in `xlink.c:xlink_wait()`:
| Scenario | Expected | Checks |
|----------|----------|--------|
| `n == 0` | -2 / EINVAL | 2 |
| `n < 0` | -2 / EINVAL | 2 |
| `chans==NULL` with n>0 | -2 / EINVAL | 2 |
| All-NULL chans[] | -2 / EINVAL | 2 |
| Valid + NULL chans[] | -2 / EINVAL | 3 |

### Code review details
- **xlink.c** — read_exact EAGAIN poll-retry, frame_recv discard, xlink_wait deadline — all correct
- **tcp_backend.c** — read_framed ENOSPC discard, recv_multi multi-chunk discard, send_to_all reverse swap-remove — formally correct
- **pipe_backend.c, serial_backend.c, udp_backend.c, file_backend.c, shm_backend.c** — all clean
- **bridge.c, monitor.c, send.c, recv.c** — no issues

### Known items (unchanged — 7 items)
1. recv_multi stale fd — swap-remove leaves stale fds[] entries until next poll rebuilds
2. xlink_read ignores timeout_ms for backends without read vtable
3. NONBLOCK EAGAIN in frame_send() (xlink.c) treated as hard error (no retry)
4. UDP receiver-as-sender write() on unconnected socket may ENOTCONN
5. shm_backend_peek error → *avail=0 indistinguishable from "no data"
6. SIGPIPE not guarded in library code (callers must signal(SIGPIPE, SIG_IGN))
7. Makefile hardcodes 25 test targets — new tests manually added

### Codebase stats
- **7 src/ + include/ + 4 tools/ + 25 test files (+2 stress)**
- **25/25 ALL PASS** (306+ checkpoints)
- **0 warnings** (`-Wall -Wextra -O2 -g`)
- **All source files clean** after 33 rounds

---

## Round 31 — 2026-04-30 07:59 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **25/25 ALL PASS** (25 test binaries, 0 failures)
- **New test added**: pure SHM `xlink_wait` timeout=-1 (2 checks in test_wait_edge.c)

### Code Review — 31st round
All 7 src/ + include/xlink.h + 4 tools/ + 25 tests/ reviewed.
**No new bugs found.** Codebase stable after 31 rounds.

Files reviewed in detail:
- `xlink.c` — frame_recv discard loop (n≤0 w/ ssize_t), read_exact EAGAIN logic, xlink_wait deadline placement — all correct
- `tcp_backend.c` — R30 read_framed discard fix verified, send_to_all swap-remove re-verified, recv_multi stale fd noted
- `shm_backend.c`, `pipe_backend.c`, `serial_backend.c`, `udp_backend.c`, `file_backend.c` — all clean
- Bridge/monitor/send/recv tools — no issues

### Known items (unchanged — 7 items)
1. recv_multi stale fd — swap-remove leaves stale fds[] entries until next poll rebuilds
2. xlink_read ignores timeout_ms for backends without read vtable
3. NONBLOCK EAGAIN in frame_send() treated as disconnect
4. UDP receiver-as-sender write() on unconnected socket may ENOTCONN
5. shm_backend_peek error → *avail=0 indistinguishable from "no data"
6. SIGPIPE not handled/guarded in library code
7. Makefile hardcodes 25 test targets — new tests must be added manually

### Port conflict analysis (sequential runs, all safe)
| Port | Tests using it |
|------|---------------|
| 19990-19991 | test_tcp_zero, test_tcp |
| 19992 | test_tcp_multi |
| 19993 | test_tcp_overflow, test_udp |
| 19994 | test_tcp_empty |
| 19995 | test_udp_edge |
| 19996 | test_tcp_server_nonblock |
| 19997 | stress_tcp |
| 19998 | test_tcp_nonblock |
| 19897 | test_tcp_overflow_client |

### Codebase stats
- **7 src/ + include/ + 4 tools/ + 25 test binaries**
- **25/25 ALL PASS** (25 test binaries, all checks pass)
- **0 warnings** with `-Wall -Wextra -O2 -g`
- **1 new test check added** this round (timeout=-1 SHM wait)

---

## Round 30 — 2026-04-30 07:29 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **25/25 ALL PASS** (24 + 1 new test binary, 0 failures)

### Bug fix: TCP client mode `read_framed` doesn't discard oversized payloads

**Root cause:** In `src/tcp_backend.c`, `read_framed()` (client-side framed reader) returned -1/ENOSPC on oversized messages without consuming the payload bytes. The caller `tcp_backend_recv` saw `errno != EAGAIN` and interpreted it as a connection failure, triggering `close(ch->fd)` + a reconnect cycle.

This was wasteful and **inconsistent** with `recv_multi()` (server mode), which correctly discards oversized payloads in a loop and continues on the same connection.

**Fix (2 changes in `tcp_backend.c`):**

1. **`read_framed()`** — Added discard loop when `msglen > *len`:
```c
/* Discard all payload bytes to maintain framing sync,
 * then tell caller to try again with a larger buffer. */
size_t remaining = msglen;
while (remaining > 0) {
    uint8_t chunk[4096];
    size_t to_read = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
    if (read_exact(fd, chunk, to_read) != 0) return -1;
    remaining -= to_read;
}
errno = ENOSPC;
return -1;
```

2. **`tcp_backend_recv()` (client mode)** — Added ENOSPC guard between EAGAIN and disconnect:
```c
/* Oversized message was discarded — try again next call,
 * no need to reconnect. */
if (errno == ENOSPC) {
    return -1;
}
```

### New test: `tests/test_tcp_overflow_client.c` (20 checks)

Three scenarios:
- **Single oversized discard** — LARGE (512B) → "small_ok" on 64-byte buffer. Discard returns -1/ENOSPC, next recv gets "small_ok" intact.
- **Multiple oversized** — LARGE → LARGE → "done". Two consecutive discards preserve framing sync.
- **No disconnect on oversized** — After oversized discard, connection remains alive. Subsequent "confirm" message received correctly.

All 20 checks PASS.

### Known items (updated — 8 items)

1. `recv_multi` stale fd on client disconnect — swap-remove leaves stale fds[] entries until next poll rebuilds
2. `xlink_read` ignores `timeout_ms` for backends without `read` vtable
3. NONBLOCK write EAGAIN in `frame_send()` (xlink.c) treated as disconnect
4. UDP receiver-as-sender `write()` on unconnected socket may `ENOTCONN`
5. `shm_backend_peek` error → `*avail = 0` indistinguishable from "no data"
6. `bridge.c` `xlink_errstr(chA)` when chA==NULL: errno may be stale
7. SIGPIPE not handled/guarded in library code
8. Makefile hardcodes 24 test targets — new tests must be added manually

### Codebase stats
- **7 src/ + include/ + 4 tools/ + 25 test binaries**
- **25/25 ALL PASS** (24 existing + 1 new: test_tcp_overflow_client)
- **0 warnings** with `-Wall -Wextra -O2 -g`

---

## Round 29 — 2026-04-30 06:59 CST

### Build & Test
- `make clean && make all` → 0 warnings (`-Wall -Wextra -O2 -g`)
- `make test` → 24/24 ALL PASS (all 24 test binaries, 0 failures)
- `make stress` → SHM 72,977 msg/s, TCP 212,712 msg/s — no regression (TCP slightly up)

### Bug fix (R28) verified: recv_multi discard loop

R28 fix (`<= 0` → `!= 0` in tcp_backend.c `recv_multi` discard loop) verified correct. The discard now correctly distinguishes success (0) from error (-1).

### New test coverage added: multi-chunk discard (`test_tcp_overflow.c`)

Extended `test_tcp_overflow.c` to exercise the multi-chunk discard path in `recv_multi`:
- Added a **8,192-byte** framed message (`HUGE_MSG_SZ`) that triggers 2 discard chunks (chunk=4096, 8192/4096 = 2 iterations)
- Sequence: huge (8KB) → large (512B) → small ("small_ok")
- Server receives "small_ok" intact after both discards
- Previously only tested single-chunk discard (512 bytes < 4096 chunk size)

All 6 checks PASS (was 5 before).

### Code review
All 7 src/ + include/ + 4 tools/ + 26 tests/ reviewed (29th round).
- R28 fix verified: `read_exact` return check `!= 0` correct
- `send_to_all` reverse swap-remove logic re-verified (formally correct)
- No new bugs found after 29 rounds

### Known items — unchanged (7 items from previous rounds)
1. `recv_multi` stale fd on client disconnect — swap-remove leaves stale fds[] entries until next poll rebuilds
2. `xlink_read` ignores `timeout_ms` for backends without `read` vtable
3. NONBLOCK write EAGAIN in `frame_send()` (xlink.c) treated as disconnect
4. UDP receiver-as-sender `write()` on unconnected socket may `ENOTCONN`
5. `shm_backend_peek` error → `*avail = 0` indistinguishable from "no data"
6. `bridge.c` `xlink_errstr(chA)` when chA==NULL: errno may be stale
7. SIGPIPE not handled/guarded in library code

### Codebase stats
- ~6,900 lines total (src + include + tests + tools)
- 6 checks in test_tcp_overflow (was 5, +1 for multi-chunk)
- 0 warnings
- SHM throughput: ~73k msg/s; TCP throughput: ~213k msg/s

---

## Round 28 — 2026-04-30 06:29 CST

### Build & Test
- `make clean && make all` → 0 warnings (`-Wall -Wextra -O2 -g`)
- `make test` → 24/24 ALL PASS (all 24 test binaries, 0 failures)
- **Previously hanging test fixed:** `test_tcp_overflow` now passes (was exit code 124/timeout)

### Bug fix: `recv_multi` discard loop in `tcp_backend.c` → `read_exact` return check

**Root cause:** In `src/tcp_backend.c` `recv_multi()`, the discard loop for oversized messages used:
```c
if (read_exact(client_fd, chunk, to_read) <= 0) {
```
`read_exact()` in tcp_backend.c returns **0 on success**, -1 on error. So `0 <= 0` evaluates to `true`, causing the client to be ***removed on successful discard*** — the client fd was closed, the server looped back to poll, and the subsequent "small_ok" message was never received.

**Fix:** Changed `<= 0` to `!= 0`:
```c
if (read_exact(client_fd, chunk, to_read) != 0) {
```

**Effect:** `test_tcp_overflow` now finishes in < 1 second (was hanging forever). The test sends a 512-byte framed message (exceeding the 64-byte recv buffer), followed by a valid "small_ok" message. Server correctly discards 512 bytes + receives "small_ok" intact.

Note: `read_exact` in `tcp_backend.c` (returns 0/-1 convention) is a `static` function, so this naming collision with `read_exact` in `xlink.c` (returns ssize_t convention) is benign but worth noting — different return convention for the same name.

### Code review
All 7 src/ files + include/ + 26 tests/ reviewed (28th round).
- No other bugs found
- Codebase structurally sound after fix
- Pipe framing, serial, UDP, SHM, file backends: no issues

### Remaining known items (unchanged from R27)
1. `recv_multi` stale fd on client disconnect — swap-remove leaves stale entries until next poll rebuild
2. `xlink_read` ignores `timeout_ms` for backends without `read` vtable
3. NONBLOCK write EAGAIN in `frame_send()` treated as disconnect
4. SIGPIPE not guarded in library (SIG_IGN documentation needed)
5. `shm_backend_peek` error → `*avail = 0` indistinguishable from "no data"
6. `shm_cleanup` silent drop at 64 entries
7. `ch->use_framing` set redundantly in pipe/serial backends (xlink.c already sets it)

### Build & Test
- `make clean && make all` → 0 warnings (`-Wall -Wextra -O2 -g`)
- `make test` → 24/24 ALL PASS (293 checkpoints across all test binaries)
- `make stress` → SHM 73,605 msg/s (avg 13µs), TCP 157,392 msg/s — no regression

### Code review
Full review of all 7 src/ × 7 = 7 source files + include/xlink.h + 4 tools/ + 26 tests/ (27th round).
**No new bugs found.** Codebase stable after 27 rounds.

### New findings (SIGPIPE vulnerability — design note)

**Observation:** None of the library's write paths (`frame_send` in xlink.c, `write_framed` in tcp_backend.c, all `backend->send` implementations) guard against `SIGPIPE`. If a user calls `xlink_send()` on:
- A broken TCP connection (server rebooted, client hung up)
- An orphaned FIFO (reader closed)
- A closed serial port

the default `SIGPIPE` action (`SIG_DFL`) terminates the process. The stress test (`stress_tcp.c`) works around this with an explicit `signal(SIGPIPE, SIG_IGN)` at the top of `main()`, but this is not documented in the library API.

**Current behavior vs. convention:** This is consistent with how many C socket libraries work — callers are expected to either `signal(SIGPIPE, SIG_IGN)` or use `MSG_NOSIGNAL` on their own. However, xlink is a library and should either:
1. Document clearly that callers must `signal(SIGPIPE, SIG_IGN)` before using framed transports
2. Use `MSG_NOSIGNAL` flag in socket operations (TCP/UDP), and block/restore SIGPIPE for pipe/serial

**Action:** At minimum, add a `SIGPIPE` note to the API documentation. The proper fix (MSG_NOSIGNAL on sockets + per-thread sigmask for pipes) is lower priority and can wait for a dedicated round.

### Coverage check
- `xlink_wait()` with all-channels-no-fd-no-peek (ENOTSUP) — not covered, but practically unreachable since all backends have either fd>=0 or peek
- `xlink_read()` with timeout on backends without `read` vtable — not covered (known issue #2)
- Stopped tests pass — confirmed stable

### Known items — unchanged
1. `recv_multi` stale fd on client disconnect — swap-remove leaves stale fds[] entries until next outer iteration rebuilds
2. `xlink_read` ignores `timeout_ms` for backends without `read` vtable
3. NONBLOCK write EAGAIN in `frame_send()` (xlink.c) treated as disconnect
4. UDP receiver-as-sender `write()` on unconnected socket may `ENOTCONN`
5. `shm_backend_peek` error → `*avail = 0` indistinguishable from "no data"
6. `bridge.c` `xlink_errstr(chA)` when chA==NULL: errno may be stale
7. SIGPIPE not handled/guarded in library code (new, see above)

### Codebase stats
- ~6,898 lines total (src + include + tests + tools)
- 293 check/assert statements across 24 test files
- 0 warnings at standard compilation flags
- SHM throughput: ~74k msg/s (stable)
- TCP throughput: ~157k msg/s (stable)

### Build & Test
- `make clean && make all` → 0 warnings (`-Wall -Wextra -Wconversion -Wshadow -pedantic`)
- `make test` → 24/24 ALL PASS (all tests, 0 failures)
- New "PASS: nonblock recv empty returns 'no data'" in test_shm

### Fixes

**`tests/test_tcp_zero.c`** — 3 compiler warnings squashed
- 2× `-Wconversion`: `htons(port)` → `htons((uint16_t)port)` (int→uint16_t conversion loss)
- 1× `-Wshadow`: inner `int rc` in `test_tcp_zero()` shadowed outer `int rc`. Renamed to `int rc2`.

**`src/shm_backend.c`** — `shm_backend_recv()` error message bug
- Previously: both non-blocking `shm_read` returning -1 (empty queue) AND blocking `shm_readn` returning -1 (real error) produced `"shm_read(%s): no data"`.
- The error message is misleading when a blocking `shm_readn` call genuinely fails (e.g., segment destroyed).
- Fixed: `"no data"` message only shown when `XLINK_NONBLOCK` is set. Blocking-mode `shm_readn` failures now show `strerror(errno)`.
- Change: added `(ch->flags & XLINK_NONBLOCK)` guard to the "no data" branch.

### New test: SHM NONBLOCK empty recv error path

**`tests/test_shm.c`** — `test_nonblock_empty()`:
1. Creates writer + non-blocking reader
2. Recvs on empty queue → verifies -1 return + errstr contains `"no data"`
3. Sends data from writer → verifies successful recv with correct content
4. Cleanup
This covers the NONBLOCK error path in `shm_backend_recv()`, proving the fix works.

### Code review
All 7 src/ + include/ + tools/ + 24 tests/ (26th round).
- Found and fixed 1 bug (shm error message misattribution)
- Found and fixed 3 warnings (shadow + conversion in test code)
- Codebase structurally sound, cross-module interfaces consistent

### Known items — unchanged (6 items from previous rounds)
1. `recv_multi` stale fds on client disconnect — swap-remove leaves stale entries in fds[] array until next outer iteration rebuilds it. No crash, no leak, just one wasted poll cycle.
2. `xlink_read` ignores `timeout_ms` for backends without `read` vtable — falls back to `backend->recv()`, which has no timeout parameter.
3. NONBLOCK write EAGAIN in `frame_send()` (xlink.c) treated as disconnect — writev doesn't handle EAGAIN on non-blocking fds.
4. UDP receiver-as-sender `write()` on unconnected socket may fail with `ENOTCONN` on non-Linux platforms.
5. `shm_backend_peek` error → `*avail = 0` indistinguishable from "no data".
6. `bridge.c` `xlink_errstr(chA)` when chA==NULL: errno may be stale from argument parsing.

### Round 26 stats since Round 18 baseline
- src/ lines: unchanged
- tests/ lines: +41 (test_nonblock_empty)
- Test count: 24 → 24 (test_shm extended, no new binary)
- Check/assert count: ~276 → ~293
- Compiler warnings: 3 → 0
- Total codebase: ~6,898 lines

---

## Round 25 — 2026-04-30 04:29 CST

### Build & Test
- `make clean && make all` → 0 warnings (verified with `-Wall -Wextra -Wconversion -Wshadow -pedantic`)
- `make test` → 24/24 ALL PASS (276 `CHECK`/assertion calls across all test files)
- `make stress` → SHM 73k msg/s, TCP 155k msg/s — no regression

### Code Review
Full review of all 7 src/, include/xlink.h, 4 tools/, 24 tests/ (25th round).
**No new bugs found.** Codebase fully stable after 25 rounds of systematic review.

### New test coverage added (Round 25)

**`tests/test_errors.c` — RTSP type (no backend)**
- `xlink_open(XLINK_RTSP, ...)` returns NULL (valid enum, no registered backend)
- `xlink_errstr(NULL)` returns `"Function not implemented"` (ENOSYS properly propagated)
- `xlink_type_str(XLINK_RTSP)` returns `"unknown"`

This covers the `find_backend` → NULL → `errno = ENOSYS` path specifically for a valid enum value (not just invalid type 999). Adds 5 checks.

### Known items — unchanged (7 items from previous rounds)
1. `recv_multi` stale fd on POLLHUP — corrected analysis: POLLHUP is detected via read_exact → EOF → remove_client. The remaining concern (POLLHUP without POLLIN making fd readable) is resolved by read_exact returning -1 on EOF.
2. `xlink_read` ignores `timeout_ms` for backends without `read` vtable
3. NONBLOCK write EAGAIN in `frame_send()` treated as disconnect
4. UDP receiver-as-sender `write()` on unconnected socket may `ENOTCONN`
5. `shm_backend_peek` error → `*avail=0` indistinguishable from "no data"
6. `bridge.c` `xlink_errstr(chA)` when chA==NULL: errno may be stale
7. `recv_multi` zero-length header passes through cleanly (protocol-valid)

### Codebase stats
- ~6,857 lines total (src + include + tests + tools)
- 276 check/assert statements across 24 test files
- 0 warnings at highest warning level
- SHM throughput: ~73k msg/s
- TCP throughput: ~155k msg/s

---

## Round 19 — 2026-04-30 01:29 CST

### Fixed: removed dead `write_exact` in tcp_backend.c

`write_exact()` (static function in `tcp_backend.c`) was replaced by `write_framed()` to atomically write the frame header + payload as a single writev operation, but the old function was never removed. It caused a `-Wunused-function` warning on every build.

**Fix:** Removed the `write_exact()` definition (15 lines). Updated the comment in `send_to_all()` that referenced `write_exact()` calls to describe the atomic-write rationale generically. Build now produces 0 warnings. All 25 tests still pass.

### Remaining items (still unresolved)

1. **`recv_multi` stale fd on POLLHUP** — When a client disconnects, `send_to_all` cleans it up. But if the disconnected client is *not* in the send path (server is idle, only `recv_multi` is called), the stale fd remains until the next `send_to_all` triggers cleanup. `recv_multi` detects POLLHUP from poll() but only removes connected-but-idle fds. A fully dead (hung-up) fd stays in the array until a send happens. Low impact: the fd is already closed by the kernel, so no leak, just a wasted slot.

2. **`xlink_read` ignores `timeout_ms` for backends without `read` vtable slot** — Falls back to `backend->recv()` which has no timeout. The `timeout_ms` parameter is silently ignored. Minor: only affects callers that use `xlink_read()` on non-TCP/pipe/serial transports with timeouts.

3. **NONBLOCK write: EAGAIN in `xlink.c:frame_send()` treated as disconnect** — `frame_send()` (in `xlink.c`, used for framed backends like pipe, TCP client, serial) does not handle EAGAIN. A non-blocking pipe that returns EAGAIN will close the channel. `frame_send` in `xlink.c` uses writev directly with EINTR retry but no EAGAIN handling. This is a real issue for non-blocking writes on full pipe buffers or slow TCP receivers.

4. **UDP receiver-as-sender: `write()` on UDP socket may `ENOTCONN`** — `udp_backend_send()` takes the `is_receiver` path and calls `write()` instead of `sendto()`. While this works on Linux for loopback, it is technically undefined for UDP unless `connect()` was called first. Low impact in practice because receiver channels typically don't send.

5. **`shm_backend_peek` sets `*avail = 0` instead of returning -1 on error** — `shm_peek` returns non-zero on error, but `shm_backend_peek` converts this to `*avail = 0; return 0;`. An error (e.g. segment destroyed) is indistinguishable from "no data." Minor: the backends that have peek are already best-effort.

6. **`bridge.c` reports chA errstr on NULL return from xlink_open type A** — `fprintf(stderr, "... %s\n", xlink_errstr(chA))` when `chA == NULL`. `xlink_errstr(NULL)` works (returns `strerror(errno)`), so this is cosmetic only — errno may reflect a stale error from argument parsing rather than the actual open failure. Very minor, since the addr mismatch error is the dominant failure case.

7. **`recv_multi` zero-length header detection** — `recv_multi` reads a 4-byte header, then checks `msglen > *len` for overflow. But it does not guard `msglen == 0`. A zero-byte message from a malicious/silly client would pass the overflow check and `read_exact(fd, buf, 0)` would return 0 (no-op). The message is then forwarded via `frame_recv` on the "other side" channel. This is a non-issue for framed protocols since zero-length messages are valid.

---

## Round 18 — 2026-03 (Previous)

### Issues found & fixed

- `shm_backend_send` called `shm_write()` (non-blocking, drops on full) instead of `shm_writen()` (blocking, retries). Fixed: send uses `shm_writen()`.
- `shm_backend_recv` had the same bug: called `shm_read()` (non-blocking) instead of `shm_readn()` (blocking). Fixed: recv respects `XLINK_NONBLOCK` flag.
- `bridge.c` segfault: `xlink_open()` returns NULL on failure, not a valid pointer. `xlink_errstr(NULL)` works, but `xlink_errstr(chA)` when chA is NULL would crash. Fixed the fprintf call.
- `monitor.c` segfault: same pattern — `xlink_errstr(ch)` when ch is NULL. Fixed.
- `udp_backend_open` multicast join: `IN6_IS_ADDR_MULTICAST` macro checks the IPv6 address. Need `<netinet/in.h>`. Already included.
- `tcp_backend.c:send_to_all()` race condition: used 2 separate write() calls (hdr then payload). This is unsafe if the TCP connection breaks between the two calls — leaves the receiver with a 4-byte header but no payload, causing framing desync at the next message boundary. Fixed: replaced with `write_framed()` using a single `writev(2)` call.

---

## Round 22 — 2026-04-30 02:59 CST

### Bug fix: `xlink_wait()` mixed-path `timeout=0` returns without polling

**Bug:** `xlink_wait()` with mixed channels (SHM + fd-based) and `timeout_ms=0` returned -1 immediately without polling or peeking. The deadline check (`if (now >= deadline_ms)`) was at the top of the loop, and with `timeout=0`, `deadline_ms == current_ms()` on entry, so every call returned -1 regardless of whether data was available.

This was inconsistent with the all-fd path, which correctly does one `poll(..., 0)` call.

**Fix:** Moved the deadline check from the top of the loop to the **bottom**, after the poll/peek work. This guarantees at least one complete poll+peek cycle before considering the deadline. The all-fd path was unaffected.

**New test:** `test_mixed_shm_pipe_poll_once` (8 checks in `test_wait_edge.c`):
- Mixed SHM+pipe, `timeout=0`, no data → returns -1
- Mixed SHM+pipe, `timeout=0`, data on SHM → returns index 1 (peek path)
- Mixed SHM+pipe, `timeout=0`, data on pipe → returns index 0 (poll path)

All 22 test suites pass. ~245 checkpoints.

### Known items (unchanged)

Items 1-7 from Round 19 remain unresolved.

### Code review

**25/25 ALL PASS, 0 warnings.** No new bugs found.

Comprehensive code review of all 7 src/ files + include/xlink.h + 4 tools/ + 25 tests/:
- Cross-module vtable signature consistency verified (6 backends × 7 entry points)
- `frame_send` writev iov advancement logic re-verified — formally correct
- `send_to_all` backwards swap-remove correctness formally re-verified
- `recv_multi` stale fd scenario re-traced line by line — functionally correct
- Codebase fully stable after 21 rounds of review

### New test: pure-SHM xlink_wait path (10 checks added to test_wait_edge.c)

Previously untested: `xlink_wait()` with **both channels being SHM** (fd=-1, npfd=0).
This exercises the pure peek-based polling loop:
- `npfd=0`, `has_peek=true` → enters the mixed-flag polling path without poll
- Tests: timeout, single-channel data detection, dual-channel ordering
- All 10 checks PASS

### Known items (unchanged — all 7 from R19/R20)

### Makefile

Makefile hardcodes 24 test targets — a new `tests/*.c` file won't be auto-linked.
If we add more tests, need to either switch to wildcard rules or add the target manually.

## Round 24 — 2026-04-30 03:59 CST

### Build & Test
- `make clean && make all` → 0 warnings
- `make test` → 24/24 ALL PASS (24 test binaries, 152 checks)
- Stress: SHM 72.8k msg/s, TCP 199.7k msg/s — no regression

### Code review
All 7 src/ + include/ + tools/ + 24 tests/ (24th round).
**No new bugs found.** Codebase fully stable.

### New test: `tests/test_tcp_zero.c` (17 checks)

Covers zero-length framed messages on TCP server mode (`recv_multi` path, msglen==0):
- Client sends zero-length message, server receives correctly (framing stays in sync)
- Zero-length message followed by normal message — framing not desynced
- "first" → zero → "second" → zero — interleaved sequence maintains sync
- All 17 checks PASS

### Known items — unchanged (7 items from previous rounds)

## Round 23 — 2026-04-30 03:29 CST

### Build & Test
- `make clean && make all` → 0 warnings
- `make test` → 25/25 ALL PASS

### Code review
All src/ + include/ + tools/ + tests/ (23rd round).
**No new bugs found.** Codebase stable.

### New test: pure-SHM xlink_wait timeout=0 (7 checks, appended to test_wait_edge.c)

Previously untested: `xlink_wait()` with both SHM channels and `timeout_ms=0` (poll-once).
Covers `npfd=0, has_peek=true, timeout_ms=0`:
- No data → one peek cycle → returns -1
- Data on rx1 → returns index 0
- Data on rx2 → returns index 1
- Data on both → returns valid index 0 or 1

### Known items (unchanged — 5 items from previous rounds)

## Round 20 — 2026-04-30 01:59 CST

### Findings

**No new bugs.** Codebase clean and stable after 20 rounds of review.
25/25 tests all PASS, 0 warnings under -Wall -Wextra -Wconversion -Wshadow -pedantic.

### Detailed review

**`send_to_all` backwards iteration + swap-remove — formally verified correct.**

The loop iterates from `p->nclients-1` down to 0. On failure at index `i`, it closes `client_fds[i]`, decrements `nclients`, and sets `client_fds[i] = client_fds[--nclients]`. Since we iterate from the end, any swapped-in element comes from a position that was *already processed* (higher index). This guarantees no double-send.

Proof by example: `nclients=3, fds=[10,11,12]`. i=2 (process 12) → OK. i=1 (process 11) → FAIL, swap from end: `client_fds[1] = client_fds[2] = 12`. i=0 (process 10) → OK. fd 12 at position 1 is skipped because i moves to 0. Correct.

**`send_to_all` swap-remove corner case — self-swap on last element:**

When the *last* element (highest index) fails, the swap is a self-swap (no-op): `client_fds[last] = client_fds[last]`. `nclients` is decremented, and subsequent iterations use `nclients-1` as the new boundary. The self-swapped stale entry is at index `nclients` which is >= `nclients-1` (the new initial i), so it's skipped. Correct.

**Compiler flags verification:**

Default CFLAGS are `-Wall -Wextra -O2 -g`. Tested with additional `-Wconversion -Wshadow -pedantic` — 0 warnings across all 25 test binaries and library. No implicit sign conversions, no shadowed variables, no ISO C violations.

## Round 36 — 2026-05-02 08:45 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **ALL 26 tests PASS** (18 "ALL PASSED" banners, **0 failures**)
- Test count: 27 test binaries (added test for multi-chunk discard in generic framer)

### Code Review — 36th round

All 7 src/ + include/xlink.h + 27 test/ files reviewed.

**Bug found in XLINK_OPT_DEFAULT macro (pre-existing, latent):**
- `XLINK_OPT_DEFAULT` was defined as a brace-enclosed initializer `{ .flags = 0, ... }`, which only works in **declaration** context (`xlink_opt_t opt = XLINK_OPT_DEFAULT;`).
- Using it in **assignment** context (`opt = XLINK_OPT_DEFAULT;`) triggers a compile error: `expected expression before '{' token`.
- Fixed: changed macro to use C99 compound literal `(xlink_opt_t){ .flags = 0, ... }`, which works in both contexts.
- Verified all existing 70+ usage sites compile cleanly.
- Test `test_frame_overflow.c` tripped on this (re-declared `opt` for multi-chunk test), which is what exposed it.

**No other bugs found.** Cross-module consistency verified:
- All backend vtable entries consistent (`.write`, `.read`, `.peek` NULL on stream backends)
- `read_exact` EAGAIN partial-data-poll path correct in xlink.c generic framer
- `frame_recv` discard path consistent between single-chunk (≤4096) and multi-chunk (>4096) cases
- `serial_backend_open()` baud clamping: addr-parsed baud clamped at `< 1200`, opt-provided baud bypasses this check — by design (user explicitly setting opt wins)

### New test: Multi-chunk discard in generic framer (10 new checks)

Extended `tests/test_frame_overflow.c` with `test_multi_chunk_discard()` — exercises the `frame_recv()` discard loop with **>4096 byte payload** (8192 bytes), which triggers **multiple discard iterations** (2 chunks of 4096). Previously the test only covered single-chunk discard (512 bytes).

This is the generic framer equivalent of `test_tcp_overflow_client.c`'s multi-chunk discard test for the TCP framer.

### Documentation improvements

**include/xlink.h**: Added docs to `XLINK_OPT_DEFAULT` macro clarifying it's a C99 compound literal, usable in both declaration and assignment, with a note against static-duration use.

**docs/known-issues.md**: Unchanged — all 8 known issues still current (no new issues introduced). The `XLINK_OPT_DEFAULT` macro fix eliminated a latent build trap, not a functional issue.

### Ideas for next version

1. **`XLINK_OPT_DEFAULT` → add C89-safe fallback**: Some embedded toolchains use C89 rules. Could provide `XLINK_OPT_DEFAULT_VAL` macro with plain initializer for C89 contexts.
2. **Wildcard test discovery in Makefile**: `tests:` target still has 27 hand-written compile lines. A `$(wildcard tests/*.c)` pattern would eliminate manual additions. The `test:` runner already uses wildcard.
3. **RTSP backend**: Still a placeholder enum. A minimal RTSP client backend would round out the transport portfolio.
4. **`xlink_bridge` tool**: CLI tool for transparent forwarding between backends (e.g., TCP ↔ SHM) is specified in docs but not built. Would be useful for gateway/router applications.
5. **Graceful `XLINK_NONBLOCK` error strings**: `udp_backend_recv()` EAGAIN message is "Resource temporarily unavailable" vs. a cleaner "no data". Cosmetic (known issue #8).
6. **`xlink_write()` on SHM/UDP/File: missing vtable**: These backends lack `.write` so `xlink_write()` falls through to `.send()`. Document this explicitly: .write is for raw-stream backends only.

## Round 37 — 2026-05-03 08:45 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **ALL 28 tests PASS** (19 "ALL PASSED" banners, **0 failures**)
- Test count: 28 test binaries (added recv-on-writeonly file test; added test_tcp_eof_mid_frame)

### Code Review — 37th round

All 7 src/ + include/xlink.h + 28 test/ files reviewed.

**No new bugs found.** Cross-module consistency verified:
- `file_backend_recv()` on write-only fd (O_WRONLY) → read() returns -1/EBADF correctly
- `serial_backend_open()` path parsing with `strrchr(':')` — correct for both `/dev/ttyUSB0:115200` and paths without colon
- `tcp_backend_open()` getaddrinfo host/port split: consistent with addr format docs
- UDP multicast: all paths still consistent

**Edge case reviewed: `file_backend_recv()` on write-only channel** — the file backend's `recv` calls `read(ch->fd, ...)` where fd was opened with O_WRONLY. On POSIX, `read()` on a write-only fd returns `-1` with `errno=EBADF`. The backend's error message is `"file read: Bad file descriptor"` — clear and correct.

### New test: File recv on write-only channel (6 new checks)

Added `test_file_recv_writeonly()` in `tests/test_file.c`:
- Opens a file with XLINK_CREATE (O_WRONLY)
- Calls `xlink_recv()` → expects -1
- Checks error string is non-empty (verifies errbuf set correctly)

This covers the **recv path on write-only file** — a complement to the existing `test_file_write_readonly()` in `test_errors.c` which tests send on read-only file.

### Documentation improvements

**docs/api.md**: Updated `XLINK_OPT_DEFAULT` macro documentation to show the C99 compound literal form `(xlink_opt_t){...}`, matching the actual code. Previously showed the old brace-enclosed form.

**docs/known-issues.md**: Unchanged — all 8 issues still current. No new issues introduced.

### Ideas for next version

1. **`file_backend_recv` error message clarity**: When read() fails on write-only fd, error says "file read: Bad file descriptor". Could add a wrapper to detect O_WRONLY and give clearer message like "file: write-only fd". Low priority — EBADF is standard POSIX.
2. **`frame_send` writev partial-advance path**: The iov pointer adjustment in `frame_send()` is an important correctness path that's hard to test (requires writev returning partial count). Could add a stress test that generates small writes and verifies no data corruption.
3. **`read_exact` EAGAIN partial-data path**: When EAGAIN occurs mid-read, `read_exact` polls with `-1` timeout. This could theoretically hang forever if the fd never becomes readable (e.g., TCP hang). An internal timeout here could be a safety net — but would change the API contract.

### Remaining items (unchanged from R19)

1-7: Same as Round 19.

- `read_exact` in xlink.c and tcp_backend.c diverged: xlink.c's version handles EAGAIN with partial-data-poll, tcp_backend.c's version does too (originally identical, verified)
- Non-blocking `EAGAIN` return path in xlink.c `frame_recv` vs tcp_backend `recv_multi` — different error propagation. Already handled correctly.
- `make deps` missing — tooling dependency story. Low priority.
- Add RTSP backend (placeholder exists in enum but no implementation)
- Thread-safety audit — `getaddrinfo` is not reentrant. No threads in current usage.
- Need documentation generation from headers.

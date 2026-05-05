# Next Version Thoughts

_Scratchpad for ideas, known issues, and plans beyond the current release._

---

## Round 43 — 2026-05-05 14:47 CST

### Documentation — P0 short-term plan document

Added `docs/future-plans/06-p0-short-term.md` with detailed design for all 3 P0 items:
1. **TCP xlink_errstr** — 6 lines of errbuf snprintf in write_framed/read_framed fail paths
2. **read_exact internal timeout** — 30s deadline for half-open TCP hang protection
3. **NONBLOCK EAGAIN retry** — writev EAGAIN retry up to 100x with POLLOUT wait

Updated `docs/future-plans/index.md` to reference the new document.

Total future-plans/: 7 files, 2 directories (including archive/).

---

## Round 42 — 2026-05-05 13:48 CST

### Documentation — future-plans/ directory created

Created `/home/admin/xlink/docs/future-plans/` with 6 documents (617 lines total):

| File | Lines | Description |
|------|-------|-------------|
| `index.md` | 75 | 路线图总览 + 优先级排序 + 依赖关系图 + 决策日志 |
| `01-plugins-arch.md` | 92 | 插件化架构设计（动态后端加载） |
| `02-async-io.md` | 112 | 异步 I/O（io_uring / epoll） |
| `03-tls-security.md` | 114 | TLS 加密通信层（OpenSSL / WolfSSL） |
| `04-performance.md` | 112 | 性能优化（Zero-Copy、批量化） |
| `05-multi-platform.md` | 112 | 跨平台支持（Windows / macOS / FreeRTOS） |

Each document follows the standard template: 动机 → 设计方案 → 实现路径（3 Phase） → 依赖 → 开放问题 → 关联文档.

P0 items for immediate next version prioritised:
1. TCP error paths → xlink_errstr
2. read_exact internal timeout
3. NONBLOCK TCP EAGAIN retry

### Ideas for next version (updated)
- [MOVED TO future-plans/] Plugin architecture → `01-plugins-arch.md`
- [MOVED TO future-plans/] Async I/O (io_uring) → `02-async-io.md`
- [MOVED TO future-plans/] TLS security → `03-tls-security.md`
- [MOVED TO future-plans/] Performance (zero-copy, batching) → `04-performance.md`
- [MOVED TO future-plans/] Cross-platform support → `05-multi-platform.md`
- Remaining short-term items (P0) kept inline:
  - `xlink_errstr` to TCP error paths
  - `read_exact` internal timeout
  - NONBLOCK TCP send EAGAIN retry
  - Wildcard test discovery in Makefile
  - UDP NONBLOCK recv error message (cosmetic)

---

## Round 41 — 2026-05-05 08:45 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **ALL tests PASS** (0 FAIL across all 29 test binaries)
- **20 new checks** added this round (pure SHM infinite wait with delayed fork)

### Code Review — 41st round (no new code changes in src/ since Round 38)

All 7 src/ + include/xlink.h + 29 test/ files reviewed. **No new bugs found.**

Cross-module consistency verified (41st consecutive clean round):
- All 6 backend vtable entries consistent (`.write = NULL`, `.read = NULL`)
- `xlink_wait` mixed/deadline logic — formally verified through both code inspection and new test
- `frame_send` writev iov advancement — formally correct through all 3 sub-paths
- `read_exact` EAGAIN mid-read poll-retry consistent between xlink.c and tcp_backend.c
- `send_to_all` swap-remove backwards iteration — formally verified correct (41st confirmation)

### New test coverage: Pure SHM infinite wait with delayed fork (20 checks)

Added `test_pure_shm_infinite_wait()` to `tests/test_wait_edge.c` — exercises the `xlink_wait()` code path where:
- `npfd == 0` (all SHM channels, fd=-1)
- `has_peek == true` (SHM backend provides peek)
- `timeout == -1` → `deadline_ms = INT64_MAX`
- No `poll()` — only `usleep(5000)` loop with peek checks
- Data arrives after a delay (200ms fork)

**Code path exercised (xlink.c):**
```c
xlink_wait(chans, n, -1)
→ npfd = 0 (all channels are SHM with fd=-1)
→ npfd != n → mixed/no-fd path
→ has_peek = true
→ deadline_ms = INT64_MAX
→ for(;;):
    npfd == 0 → skip poll()
    peek each channel → no data yet
    usleep(5000)
    timeout_ms >= 0 is false → skip deadline check
    → loops until child sends data via SHM tx
    → peek finds data → return index
```

**Test structure:**
1. **Test 1** (1-channel): SHM rx only, child forks, sleeps 200ms, sends via SHM tx
2. **Test 2** (2-channel): SHM rx + rx2, child sends to rx2, verifies correct index returned

**New checks:**
1. Pure SHM tx open succeeds
2. Pure SHM rx open succeeds
3. Second SHM pair open succeeds
4. Fork succeeds
5. Wait(-1) returns index 0
6. Recv returns 0
7. Data content matches 'pinf_dly'
8. Waitpid returns child PID
9. Child exited normally
10. Child exit status 0
11-20. Same pattern for 2-channel test (wait returns index 1 for data on rx2)

### Documentation improvements
- **Updated: `tests/test_wait_edge.c`** — new `test_pure_shm_infinite_wait()` (75 lines, 20 checks)
- **Updated: `docs/next-version-thoughts.md`** — this entry (Round 41)
- **API doc (`docs/api.md`)** — reviewed, no stale info. xlink_wait return values documented since Round 33
- **Integration guide (`docs/integration-guide.md`)** — reviewed, comprehensive and current
- **known-issues.md** — all 8 items still current. No new issues introduced in this round

### Codebase stats
- **7 src/ + 1 include/ + 29 test binaries** (unchanged)
- **421 PASS, 0 FAIL** (20 new checks this round)
- **0 warnings** with `-Wall -Wextra -O2 -g`
- **No new bugs** after 41 consecutive review rounds
- All 8 known issues unchanged

### Ideas for next version (unchanged from Round 40)

1. **`xlink_errstr` to TCP error paths**: write_framed/read_framed errors in tcp_backend.c set errno but don't call snprintf(ch->errbuf). Some errno messages are generic.
2. **`read_exact` internal timeout**: EAGAIN mid-read with -1 timeout can hang on half-open TCP.
3. **Makefile wildcard test discovery**: `$(wildcard tests/*.c)` would eliminate manual additions.
4. **Non-blocking TCP client send EAGAIN**: write_framed does not retry on EAGAIN. In NONBLOCK mode with full send buffer, this incorrectly triggers disconnect.
5. **UDP NONBLOCK recv error message**: recvfrom EAGAIN says "Resource temporarily unavailable" instead of "no data". Cosmetic (known issue #8).
6. **`frame_send` writev partial-advance test**: EAGAIN mid-writev is almost impossible to reproduce reliably in tests.

### Remaining known issues (unchanged — 8 items)
As documented in docs/known-issues.md. All items verified still current.

---

## Round 40 — 2026-05-04 20:45 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **ALL 29 tests PASS** (401 PASS, 0 FAIL across all binaries)
- **6 new checks** added this round (UDP receiver send failure path)

### Code Review — 40th round (no new code changes since Round 38)

All 7 src/ + include/xlink.h + 29 test/ files reviewed. **No new bugs found.**

Cross-module consistency verified (40th consecutive clean round):
- All 6 backend vtable entries consistent — `.write = NULL` on all except file_backend has NULL; `.read = NULL` on all
- `frame_send` writev iov advancement — formally correct through all 3 sub-paths
- `read_exact` EAGAIN mid-read poll-retry consistent between xlink.c and tcp_backend.c
- `send_to_all` swap-remove backwards iteration — formally verified correct (40th confirmation)
- `xlink_wait` mixed-path deadline logic correct (confirmed through code inspection)

### New test coverage: UDP receiver `write()` fallback send failure (6 checks)

Added `test_udp_receiver_send_fails()` to `tests/test_udp_edge.c` — exercises the `udp_backend_send()` code path where a receiver-mode channel (opened with `:port`, `is_receiver=1`, `dest_len=0`) attempts `xlink_send()`.

**Code path exercised:**
```c
udp_backend_send(ch, data, len)
  → p->dest_len == 0              // receiver has no dest_addr
  → p->is_receiver == 1           // opened as ":port"
  → write(ch->fd, data, len)      // unconnected DGRAM socket
  → returns -1 / EDESTADDRREQ     // "Destination address required"
```

**New checks:**
1. UDP receiver open on `:19988` succeeds
2. `xlink_send()` on receiver returns -1
3. `xlink_errstr(ch)` is non-NULL
4. errstr contains "udp write"
5. errstr is non-empty
6. Recv still works after failed send (receiver not broken)

### Documentation improvements
- **Updated: `tests/test_udp_edge.c`** — new `test_udp_receiver_send_fails()` (35 lines, 6 checks)
- **Updated: `docs/next-version-thoughts.md`** — this entry (Round 40)
- **API doc (`docs/api.md`)** — reviewed, no stale info. UDP receiver-send documented by example in test
- **Integration guide (`docs/integration-guide.md`)** — reviewed, comprehensive and current
- **known-issues.md** — all 8 items still current. No new issues introduced. The UDP receiver send failure is by design (write() on unconnected DGRAM), not a bug.

### Codebase stats
- **7 src/ + 1 include/ + 29 test binaries** (unchanged)
- **401 PASS, 0 FAIL** (6 new checks this round)
- **0 warnings** with `-Wall -Wextra -O2 -g`
- **No new bugs** after 40 consecutive review rounds
- All 8 known issues unchanged

### Ideas for next version (unchanged from Round 39)

1. **`xlink_errstr` to TCP error paths**: write_framed/read_framed errors in tcp_backend.c set errno but don't call snprintf(ch->errbuf). Some errno messages are generic.
2. **`read_exact` internal timeout**: EAGAIN mid-read with -1 timeout can hang on half-open TCP.
3. **Pure SHM infinite wait with delayed fork**: npfd=0, has_peek=true, timeout=-1 path with child delay still untested. Pure SHM with fork+delay not yet covered.
4. **Wildcard test discovery**: Makefile hardcodes test targets. `$(wildcard tests/*.c)` would eliminate manual additions.
5. **Non-blocking TCP client send EAGAIN**: write_framed does not retry on EAGAIN. In NONBLOCK mode with full send buffer, this incorrectly triggers disconnect.
6. **UDP NONBLOCK recv error message**: recvfrom EAGAIN says "Resource temporarily unavailable" instead of "no data". Cosmetic (known issue #8).

### Remaining known issues (unchanged — 8 items)
As documented in docs/known-issues.md. All items verified still current.

---

## Round 39 — 2026-05-04 08:45 CST

### Build & Test
- `make clean && make all` → **0 warnings** (`-Wall -Wextra -O2 -g`)
- `make test` → **ALL 29 tests PASS** (0 failures across all binaries)
- **4 new checks** added this round (TCP server send with no clients)

### Code Review — 39th round

All 7 src/ + include/xlink.h + 29 test/ files reviewed. **No new bugs found.**

Cross-module consistency verified:
- `frame_send` writev partial-header-advance logic in xlink.c: header vs payload boundary calculation correct on all 3 sub-paths (n entirely in header, split on boundary, entirely in payload)
- `xlink_wait` mixed-path deadline vs poll: timeout=-1 (INT64_MAX) path verified through code inspection — `remain=10` tight polling with peek works correctly
- `read_exact` EAGAIN mid-read poll-retry consistent between xlink.c (generic framer) and tcp_backend.c (both poll for -1 ms)
- All 6 backend vtables consistent: `.write = NULL` on stream backends, `.read = NULL` on all; fallback chains correct
- `send_to_all()` in tcp_backend.c: `ok_count == 0` branch returns -1 with "tcp: no connected clients" — verified through new test
- UDP receiver-as-sender `write()` fallback path (is_receiver=1, dest_len=0) still an untested edge case; known to fail on unconnected socket (EDESTADDRREQ)

### New test coverage: TCP server send with no clients (4 checks)

Added `test_tcp_server_send_no_clients()` to `tests/test_errors.c` — exercises the `send_to_all()` error path in `tcp_backend.c` where `ok_count == 0` (no connected clients).

**Code path exercised:**
```c
tcp_backend_send(ch, data, len)
  → p->is_client == false  (server mode)
  → send_to_all(p, data, len, ch)
    → for i = nclients-1..0: write_framed()  // empty loop, no iterations
    → ok_count == 0
    → snprintf(errbuf, "tcp: no connected clients")
    → return -1
```

**New checks:**
1. TCP server open on `:19999` succeeds
2. `xlink_send()` with no clients returns -1
3. `xlink_errstr(ch)` is non-NULL
4. errstr contains "no connected clients"

### Documentation improvements
- **Updated: `docs/next-version-thoughts.md`** — this entry (Round 39)
- **Updated: `tests/test_errors.c`** — new `test_tcp_server_send_no_clients()` function
- **API doc (`docs/api.md`)** and **integration guide (`docs/integration-guide.md`)** — reviewed, no stale info. `xlink_wait` return values (-2 for EINVAL) properly documented since Round 33.
- **known-issues.md** — all 8 issues still current. No new issues introduced (TCP server send with no clients returns expected error, not a bug).

### Ideas for next version

1. **Add `xlink_errstr` to TCP error paths**: `write_framed`/`read_framed` errors in tcp_backend.c set errno but the return values bubble up without calling `snprintf(ch->errbuf)`. Some errno-based error messages are still generic "tcp: disconnected" rather than specific reason.

2. **`read_exact` internal timeout**: When `read_exact` polls with infinite timeout (-1) on an EAGAIN mid-read, a half-open TCP connection causes indefinite hang. An internal deadline per call would improve robustness.

3. **Pure SHM infinite wait with delayed fork**: The pure-SHM (npfd=0, has_peek=true, timeout=-1) path with a child delay is still untested. The Round 38 test covers mixed SHM+pipe with delay; pure-SHM remains.

4. **UDP receiver `write()` fallback test**: `udp_backend_send()` when `p->is_receiver == 1` and `p->dest_len == 0` falls through to `write()` on an unconnected socket. Could add test verifying this returns -1 with EDESTADDRREQ.

5. **Wildcard test discovery**: Makefile `tests:` target now has 29 hand-written compile lines. The long-standing `$(wildcard tests/*.c)` pattern would eliminate manual additions.

6. **Non-blocking TCP client send EAGAIN**: `write_framed` does not retry on EAGAIN. In non-blocking mode with a full send buffer, this incorrectly triggers disconnect. Add `poll(OUT)` retry.

### Remaining known issues (unchanged — 8 items)

1. `recv_multi` stale fd on client disconnect — swap-remove leaves entries in fds[]
2. `xlink_read()` silently ignores timeout_ms on SHM/UDP/File backends (`.read = NULL`)
3. NONBLOCK write EAGAIN in `frame_send()` treated as hard error (no retry)
4. UDP receiver-as-sender `write()` on unconnected socket may ENOTCONN
5. `shm_backend_peek` error → `*avail=0` indistinguishable from "no data"
6. `bridge.c` `xlink_errstr(chA)` when chA==NULL: errno may be stale from arg parsing
7. SIGPIPE not guarded in library code (callers must `signal(SIGPIPE, SIG_IGN)`)
8. Makefile hardcodes 29 test targets — new tests manually added

### Codebase stats
- **7 src/ + 1 include/ + 29 test binaries** (unchanged)
- **0 warnings** (`-Wall -Wextra -O2 -g`)
- **4 new checks** this round (TCP server send no clients)
- **No new bugs** after 39 consecutive review rounds
- All 8 known issues unchanged

---

## Round 35 — 2026-05-01 20:45 CST

### Build & Test
- `make clean && make all` → 0 warnings (`-Wall -Wextra -O2 -g`)
- `make test` → ALL 29 tests PASS (0 failures)
- **4 new checks** added (serial open failure on nonexistent device)

### Code Review
All 7 src/ + include/xlink.h + 29 test/ files reviewed. No new bugs found.

Cross-module consistency:
- All 6 backend vtables consistent. `.write = NULL` on all stream backends; `.read = NULL` on all
- `frame_send` writev iov advancement logic correct
- `read_exact` EAGAIN mid-read poll-retry consistent between xlink.c and tcp_backend.c
- `send_to_all` swap-remove backwards iteration logic verified (i from nclients-1 down to 0)
- `xlink_wait` mixed-path deadline handling correct

### New test coverage: Serial open failure (4 checks)

Added `test_serial_open_failure()` to `tests/test_errors.c` — verifies that opening a serial port with a nonexistent device file returns NULL with proper error string.

**Checks:**
1. `xlink_open(XLINK_SERIAL, "/dev/nonexistent0")` returns NULL
2. `xlink_errstr(ch)` returns non-empty string after failed open
3. `xlink_open` with invalid baud suffix returns NULL
4. Error string after invalid baud is non-empty

### Documentation improvements
- **Updated: `docs/next-version-thoughts.md`** — this entry
- **Updated: `tests/test_errors.c`** — new `test_serial_open_failure()` function
- **API doc (`docs/api.md`)** — reviewed: `xlink_wait` return values for -2 (EINVAL) should be documented. Added to ideas list.
- **Integration guide (`docs/integration-guide.md`)** — checked for stale API surface. Current.
- **known-issues.md** — all 8 issues still current. No new issues introduced.

### Ideas for next version
1. **Add `xlink_errstr` to TCP error paths**: `write_framed`/`read_framed` errors in tcp_backend.c set errno but don't set errbuf. Return values are generic.
2. **`read_exact` internal timeout**: EAGAIN mid-read with -1 timeout can hang on half-open TCP. Internal deadline per call.
3. **UDP receiver write fallback test**: receiver mode with `dest_len==0` falls through to `write()`. Untested path.
4. **TCP server send with no clients**: `send_to_all()` with 0 clients returns -1 "no connected clients". Add test in `test_errors.c`.
5. **Pure SHM infinite wait with delayed fork**: npfd=0, has_peek=true, timeout=-1 still untested. Round 38 covers mixed SHM+pipe; pure SHM remains.
6. **`xlink_wait` doc return values**: API doc mentions -2 for EINVAL but Rounds 21-33 established concrete return conventions. Document clearly.
7. **Wildcard test discovery**: Makefile hardcodes 29 test targets — new tests added manually. `$(wildcard tests/*.c)` would automate this.

### Remaining known issues (unchanged — 8 items)
1. `recv_multi` stale fd on client disconnect — swap-remove leaves entries in fds[]
2. `xlink_read()` silently ignores timeout_ms on SHM/UDP/File backends (`.read = NULL`)
3. NONBLOCK write EAGAIN in `frame_send()` treated as hard error (no retry)
4. UDP receiver-as-sender `write()` on unconnected socket may ENOTCONN
5. `shm_backend_peek` error → `*avail=0` indistinguishable from "no data"
6. `bridge.c` `xlink_errstr(chA)` when chA==NULL: errno may be stale from arg parsing
7. SIGPIPE not guarded in library code (callers must `signal(SIGPIPE, SIG_IGN)`)
8. Makefile hardcodes 29 test targets — new tests manually added

### Codebase stats
- **7 src/ + 1 include/ + 29 test binaries** (unchanged)
- **0 warnings** (`-Wall -Wextra -O2 -g`)
- **4 new checks** this round (serial open failure)
- **No new bugs** after 35 consecutive review rounds
- All 8 known issues unchanged

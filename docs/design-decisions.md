# Design Decisions

This document captures intentional design decisions that might otherwise look
like bugs or oversights. Each entry explains the rationale and alternatives
considered.

---

## 1. Silent Oversized Message Discard

**Where**: `src/xlink.c` — `frame_recv()` discard path

**What**: When a received message exceeds the caller's buffer, the framer
silently discards the excess bytes to maintain framing sync, then returns
ENOSPC to the caller. No error message is recorded in `ch->errbuf`.

**Why**: 
- The alternative (returning partial data) would leave the next `recv()` call
  reading from the middle of a frame, causing complete desync.
- Closing the connection on oversized messages is too aggressive for a library.
- The caller gets `errno == ENOSPC` and can issue a larger buffer next call.
- Silent discard keeps the stream in sync and requires zero protocol handshake.

**Trade-off**: The caller must retry with a larger buffer. The discarded bytes
are truly lost — for critical protocols, callers should ensure buffer sizes
exceed `XLINK_MAX_MSG_SIZE`.

---

## 2. Serial Baud Rate Default to 9600

**Where**: `src/serial_backend.c` — `baud_to_speed()`

**What**: Unrecognized baud rates (e.g., 500) silently fall back to 9600.

**Why**:
- The termios API has a fixed set of `Bxxx` constants (B4800, B9600, ...).
- Raising an error for unknown baud rates would reject values that the hardware
  *might* support, forcing users to check the table before every call.
- 9600 is the most widely compatible fallback (it's the default for most serial
  devices and terminal emulators).
- The known baud set is documented inline in the source.

**Trade-off**: A mismatch between the configured and actual baud rate can cause
communication errors. If this is a concern, always specify a known rate.

**Known rates**: 4800, 9600, 19200, 38400, 57600, 115200, 230400.

---

## 3. xlink_read() Ignores timeout_ms on Backends Without .read Vtable

**Where**: `src/xlink.c` — `xlink_read()` (core), `src/tcp_backend.c` (TCP fix)

**What**: When a backend has `.read = NULL`, `xlink_read()` falls through to
`.recv()`, which has no timeout parameter. The caller's `timeout_ms` is silently
ignored and the call blocks indefinitely.

**Status**: ✅ All 6 backends fixed (2026-05-28). SHM was the last — implements `.read` via
`shm_peek()` polling (500µs interval) with `clock_gettime(CLOCK_MONOTONIC)` timeout.
(File: regular files always poll as "ready" on Linux, so timeout is effectively
ignored — same behavior as the old NULL fallback, but the path is now explicit.)

**Why it was hard to fix**:
- SHM has no fd to poll on (relies on `shm_ipc` internal signaling) — solved with peek-polling
- Pipe and Serial require fd extraction from priv struct — solved with poll() wrapper
- UDP needs framing translation — solved with recvfrom() poll wrapper
- File I/O doesn't have poll semantics — solved with poll() as "always ready"

**Workaround**:
```c
xlink_channel_t* chans[1] = { ch };
int rc = xlink_wait(chans, 1, timeout_ms);
if (rc < 0) /* timeout or error */;
xlink_recv(ch, buf, &len);
```

---

## 4. SHM atexit Cleanup Cap (256 Entries)

**Where**: `src/xlink.c` — `MAX_CLEANUP`

**What**: The atexit SHM cleanup array supports up to 256 names. Beyond that,
extra segments leak if not explicitly destroyed.

**Why**:
- The array is fixed-size (2KB BSS) to avoid dynamic allocation in `atexit`
  handlers (which run during `exit()` — unsafe to malloc).
- 256 entries covers virtually all practical use cases. Exceeding it requires
  creating 256+ named SHM channels in a single process.
- Users with extreme SHM usage should call `shm_destroy()` explicitly.

---

## 5. TCP Auto-Reconnect: Aggressive Retry

**Where**: `src/tcp_backend.c` — `try_reconnect()` / `tcp_backend_send()`

**What**: On connection loss, the client enters an exponential-backoff reconnect
loop (100ms → 200ms → 400ms → ... up to a max). The connection error message
says "lost connection (will reconnect)" without distinguishing transient vs.
permanent failures.

**Why**:
- Auto-reconnect is designed for flaky network environments where connections
  drop and come back quickly (e.g., WiFi, container restarts).
- The backoff prevents thundering herd while allowing quick recovery.
- Permanent failures (e.g., wrong port) will eventually hit the max backoff
  and keep retrying — there's no "give up" logic.
- Callers wanting explicit error handling can check the number of consecutive
  failures via `ch->errbuf` and implement their own timeout.

---

## 6. Single Header File (xlink.h) — No Internal Headers Exposed

**Where**: `include/xlink.h`

**What**: All public types, constants, and function declarations are in one
header. Backend-specific headers are internal (`src/xlink_internal.h`).

**Why**:
- Simple include story for users: `#include "xlink.h"` and done.
- Backend details (vtable layout, private structs) are implementation details.
- Single header makes it easy to vendor the library into other projects.
- Trade-off: rebuilding from sources after minor changes requires compiling all
  translation units, but for a library of this size it's negligible.

---

## 7. Test Ports Are Hardcoded, Tests Run Serially

**Where**: `tests/test_tcp_overflow.c`, `tests/test_tcp_overflow_client.c`, etc.

**What**: TCP tests use hardcoded port numbers (e.g., 19897, 19992). Tests are
run serially via `make test`.

**Why**:
- Parallel test execution would require dynamic port allocation, which adds
  complexity (port manager, environment variables, retry on EADDRINUSE).
- Serial execution is simple and reliable. With 27 tests, the total runtime
  is under 3 seconds.
- If parallel CI execution is needed later, dynamic ports can be added via
  environment variable override (e.g., `TEST_PORT_BASE=30000`).

---

## 8. No Dynamic Library Build (Static .a Only)

**What**: The Makefile builds `libxlink.a` only. No `.so` / `.dylib`.

**Why**:
- The library is small (~50KB). Static linking avoids runtime dependency
  management.
- All current users embed xlink in their applications. No shared library
  use cases exist.
- A shared library build is trivial to add (`-shared -fPIC`) when needed.

## 9. Plugin Architecture: void* Handles for ABI Stability

**Where**: `include/xlink.h` — `xlink_aio_create()`, `xlink_plugin_load()`

**What**: All v2.0 public APIs that wrap internal objects (AIO engines, plugin
registries) expose `void*` opaque handles rather than typed struct pointers.
The internal types remain in `src/` headers only.

**Why**:
- Keeps `xlink.h` ABI-stable: internal struct layout can change without
  recompiling client code.
- Users treat handles as opaque tokens — they can't accidentally depend on
  internal fields.
- Same pattern used by POSIX (e.g., `pthread_t` is opaque on most platforms).

**Trade-off**: Type safety is deferred to runtime. Passing a wrong handle
type (e.g., passing a plugin handle to `xlink_wait_aio()`) causes undefined
behavior rather than a compile error. This is acceptable because the API
surface is small (~5 new functions) and the handles are used in clearly
separated contexts.

## 10. epoll as Default Engine, poll as Fallback

**Where**: `src/aio.c` — `xlink_aio_create(0)` auto-detection

**What**: `xlink_aio_create(0)` tries `epoll_create1(EPOLL_CLOEXEC)` first;
if it fails, falls back to POSIX `poll()`. Users can still explicitly request
`xlink_aio_create(2)` for epoll or `xlink_aio_create(1)` for poll.

**Why**:
- epoll is strictly superior for large fd sets — O(1) per event vs O(n) scan.
- But poll() works on every POSIX system. The fallback means xlink compiles and
  runs on macOS/BSD without Linux-specific dependencies.
- Explicit type selection lets advanced users force a particular engine for
  benchmarking or debugging.
- io_uring (type=3) is reserved for Phase 2 step 2.7.

**Trade-off**: The auto-detection is purely compile/runtime based on kernel
support. No configure-time feature detection — intentionally simple for a
library of this size.

## 11. Per-Client TLS via Parallel Arrays (Server Mode)

**Where**: `src/tcp_backend.c` — `tcp_priv_t` and `src/tls.c` — `tls_clone_for_client()`

**What**: In TLS server mode, each accepted client gets its own `SSL` object
(stored in `client_tls[MAX_CLIENTS]`, parallel to `client_fds[MAX_CLIENTS]`),
all sharing a single `SSL_CTX` from the channel. The `recv_multi()` and
`send_to_all()` paths temporarily swap `ch->tls` to the per-client TLS state
during I/O.

**Why**:
- SSL objects are per-socket — you can't multiplex a single SSL over multiple
  fds. The naive approach (one `ch->tls` for all clients) binds to whichever fd
  was last accepted, leaving prior clients' TLS in an undefined state.
- SSL_CTX (certificates, verification policy, TLS version) IS shared — one
  server channel has one trust configuration. Cloning separate CTX per client
  would waste memory and risk divergence.
- Parallel arrays (`client_fds[i]` ↔ `client_tls[i]`) keep memory contiguous
  and avoid pointer indirection compared to a `struct { int fd; SSL *ssl; }`
  array. Cache locality matters for the poll loop.
- Temporary `ch->tls` swap means `read_framed_tls()` / `write_framed_tls()`
  don't need signature changes — purely internal to the backend.

**Trade-off**: The swap pattern (`saved_tls = ch->tls; ch->tls = client_tls[i];
...; ch->tls = saved_tls`) is not thread-safe. A concurrent thread accessing
`ch->tls` would see inconsistent state. This is acceptable because xlink
channels are single-writer/single-reader by design.

**Alternatives considered**: Passing TLS state as an extra parameter to every
framing function — cleaner architecturally but doubles the parameter count
across 4 functions. The swap pattern is uglier but contained.

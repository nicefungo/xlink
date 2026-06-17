# xlink Code Walkthrough

For experienced engineers who want to understand the full architecture.

> **Last updated:** 2026-06-17
> **Lines of code:** ~3,750 across 11 src + 2 headers + 32 test files
> **External dependency:** `libshm_ipc.a` (SHM backend only)
> **v2.0 additions:** `src/plugin.c`, `src/aio.c`, `src/aio_epoll.c`, `src/aio_poll.c`
> **v2.1 additions:** `src/aio_uring.c`, eventfd in SHM, `xlink_run()` event loop

---

## 1. File Layout

```
xlink/
├── include/xlink.h          — Public API header (opaque handle, enums, option struct)
├── src/
│   ├── xlink.c              — Core dispatch: vtable routing, framing, xlink_wait, SHM cleanup
│   ├── xlink_internal.h     — Internal shared types: xlink_backend_t vtable, xlink_channel_t
│   ├── plugin.c             — Plugin registry: register/unregister/find/load (v2.0)
│   ├── aio.c                — Async I/O engine management (v2.0)
│   ├── aio_epoll.c          — Linux epoll engine (v2.0)
│   ├── aio_poll.c           — POSIX poll fallback engine (v2.0)
│   ├── aio_uring.c          — io_uring engine, raw syscall (v2.1)
│   ├── shm_backend.c        — SHM backend: wraps shm_ipc library (name-based shared memory)
│   ├── pipe_backend.c       — Pipe backend: POSIX FIFO (named pipe)
│   ├── tcp_backend.c        — TCP backend: multi-client server + auto-reconnect client
│   ├── udp_backend.c        — UDP backend: unicast + multicast (IGMP)
│   ├── serial_backend.c     — Serial backend: RS-232/485, termios config
│   └── file_backend.c       — File backend: dump/replay of framed message sequences
├── tools/
│   ├── bridge.c             — Bidirectional forwarder between any two channels (main)
│   ├── monitor.c            — Channel listener with hex dump + stats (main)
│   ├── send.c               — Read stdin, send to channel (main)
│   └── recv.c               — Receive from channel, write to stdout (main)
├── tests/                   — 30 test_*.c files, auto-discovered by Makefile wildcard
├── third_party/shm_ipc/     — External SHM IPC library (git subtree)
└── docs/                    — See §10
```

---

## 2. Core Data Structures

There are exactly two types that matter internally:

### 2.1 `xlink_channel_t` (`src/xlink_internal.h`)

```c
struct xlink_channel {
    const xlink_backend_t* backend;   // vtable pointer — which backend owns this
    void*                  priv;      // backend-specific state (opaque to core)
    int                    fd;        // generic file descriptor, or -1 for SHM
    int                    flags;     // XLINK_CREATE | XLINK_SERVER | XLINK_NONBLOCK | XLINK_BROADCAST
    int                    use_framing; // 1 = add 4-byte length prefix on send/recv
    char                   errbuf[128]; // human-readable last error message
};
```

This is an **open struct** — backends can access fields directly. No getter/setter indirection. The `priv` pointer is cast to the backend's private struct type.

The primary dispatch axis is `ch->backend` — every public API call follows: `ch->backend->func(ch, ...)`.

### 2.2 `xlink_backend_t` (vtable, same file)

```c
typedef struct {
    xlink_type_t type;          // XLINK_SHM, XLINK_TCP, ...
    const char*  name;          // "shm", "tcp", ...
    int  (*open) (xlink_channel_t* ch, const char* addr, const xlink_opt_t* opt);
    void (*close)(xlink_channel_t* ch);
    int  (*send) (xlink_channel_t* ch, const void* data, size_t len);
    int  (*recv) (xlink_channel_t* ch, void* buf, size_t* len);
    int  (*write)(xlink_channel_t* ch, const void* data, size_t len);
    int  (*read) (xlink_channel_t* ch, void* buf, size_t len, int timeout_ms);
    int  (*peek) (xlink_channel_t* ch, size_t* avail);
} xlink_backend_t;
```

All 8 function pointers have a vtable entry. A backend may set any to `NULL` — the core provides sensible fallbacks (see §4).

### 2.3 Backend registry (`src/xlink.c`)

```c
static const xlink_backend_t* backends[] = {
    &xlink_shm_backend,
    &xlink_pipe_backend,
    &xlink_tcp_backend,
    &xlink_udp_backend,
    &xlink_serial_backend,
    &xlink_file_backend,
};
```

Static array indexed by iteration (not by enum value). `xlink_open()` calls `find_backend(type)` which does a linear scan — negligible cost for 6 entries.

---

## 3. Three Backend Categories

Every backend falls into one of three patterns:

### 3.1 Stream + automatic framing (PIPE, TCP, SERIAL)

These transports have no message boundaries — they're byte streams. The core's `frame_send()` / `frame_recv()` adds a 4-byte big-endian length prefix to every message on send, and strips it on recv.

The `ch->use_framing` flag is set in `xlink_open()` based on transport type:

```c
// in xlink_open(), src/xlink.c
ch->use_framing = (type == XLINK_PIPE || type == XLINK_TCP || type == XLINK_SERIAL);
```

When `use_framing` is true:
- `xlink_send()` → calls `frame_send()` instead of backend vtable
- `xlink_recv()` → calls `frame_recv()` instead of backend vtable
- Backend's `send`/`recv` vtable sill is NOT called for normal messaging.

**Why not use the backend send/recv for framed transports?** Because the framing layer keeps its own receive buffer state on the stack within `frame_recv()` — it reads the 4-byte header, then the payload, all in one call. This avoids a separate buffer in the channel struct.

**Exception:** The TCP backend has its own framing. When `use_framing` is set by the core, the TCP backend's `send`/`recv` vtable entries are NOT used for normal messaging — but they ARE used by the `recv_multi()` server path (see §5.3), which does its own framing inline.

Wait — that's a subtle point worth examining more closely.

### 3.2 Message-based, no framing (SHM, UDP, FILE)

These transports have natural message boundaries (shared memory rings, datagrams, or file records). `use_framing` is false. `xlink_send()` directly calls `backend->send()`, and `xlink_recv()` directly calls `backend->recv()`.

**Core difference from framed transports:** The backend itself determines what constitutes "one message." For SHM, it's one `shm_writen` / `shm_readn` pair. For UDP, it's one `sendto` / `recvfrom`. For FILE, it's one record (which internally uses the same 4-byte-length format as framing, but stored in the file).

### 3.3 Hybrid: TCP backend does its own framing

The TCP backend is unique: it sets `ch->use_framing` back to 0 after `xlink_open()` sets it to 1:

```c
// tcp_backend.c: tcp_connect_client()
ch->use_framing = 0;    // backend manages framing for reconnect
```

Why? Because the TCP backend needs **reconnect** support. When the connection drops and reconnects, the framing layer in `xlink.c` doesn't know about the new fd. The TCP backend's own `write_framed()` / `read_framed()` (in `tcp_backend.c`) handle reconnect internally (see §5.2).

This means for TCP:
- **Core `xlink_send`** calls `ch->backend->send()` → `tcp_backend_send()` → which calls local `write_framed(fd, ...)` 
- **Core `xlink_recv`** calls `ch->backend->recv()` → `tcp_backend_recv()` → which calls local `read_framed(fd, ...)`

---

## 4. Core Dispatch: xlink_send / xlink_recv

```c
// src/xlink.c
int xlink_send(xlink_channel_t* ch, const void* data, size_t len) {
    if (ch->use_framing)
        return frame_send(ch, data, len);       // core: writev(header + payload)
    return ch->backend->send(ch, data, len);     // backend vtable
}

int xlink_recv(xlink_channel_t* ch, void* buf, size_t* len) {
    if (ch->use_framing)
        return frame_recv(ch, buf, len);        // core: read_exact(4-byte header) + payload
    return ch->backend->recv(ch, buf, len);     // backend vtable
}
```

For **fallback paths** (when backend vtable entries are NULL):

```c
int xlink_write(xlink_channel_t* ch, const void* data, size_t len) {
    if (ch->backend->write)
        return ch->backend->write(ch, data, len);
    return ch->backend->send(ch, data, len);    // fallback: use send for streaming
}

int xlink_read(xlink_channel_t* ch, void* buf, size_t len, int timeout_ms) {
    if (ch->backend->read)
        return ch->backend->read(ch, buf, len, timeout_ms);
    size_t n = len;
    if (ch->backend->recv(ch, buf, &n) == 0)    // fallback: use recv, ignoring timeout
        return (int)n;
    return -1;
}
```

The `xlink_read()` fallback is a **known design limitation** (§10): calling `.recv()` instead of `.read()` drops the timeout parameter, which means `xlink_read()` with a short timeout on SHM/UDP/FILE can block indefinitely. See `docs/known-issues.md` #1 and `docs/design-decisions.md` #3.

---

## 5. TCP Backend — The Most Complex Backend

### 5.1 Open: client vs server

```
xlink_open(XLINK_TCP, "host:port")    → tcp_connect_client()
xlink_open(XLINK_TCP, ":port",        → tcp_serve_multi()
              {.flags = XLINK_SERVER})
```

**Client path** (`tcp_connect_client`):
1. Parse address into `recon_host` + `recon_port` (saved in `tcp_priv_t`)
2. `getaddrinfo()` + `socket()` + `connect()` — tries all address families
3. Sets `O_NONBLOCK` if `XLINK_NONBLOCK` flag
4. Marks `ch->use_framing = 0` (self-framing, see §3.3)

**Server path** (`tcp_serve_multi`):
1. `socket(AF_INET6)` — dual-stack IPv6 with `IPV6_V6ONLY=0` (accepts both IPv4 and IPv6)
2. `bind()` + `listen()` — backlog from `opt->tcp.backlog` (default 5)
3. Creates `tcp_priv_t` with `listen_fd` and empty `client_fds[]` (max 64)
4. Sets `O_NONBLOCK` on listen fd (non-blocking accept loop)
5. `ch->fd` = listen fd

### 5.2 Client: auto-reconnect

The client has a state machine hidden in `tcp_priv_t`:

```c
typedef struct {
    char*     recon_host;
    uint16_t  recon_port;
    int       is_client;
    int       recon_backoff;   // 0 = connected, 100..5000 ms
} tcp_priv_t;
```

When `ch->fd >= 0`: normal operation.
When `ch->fd < 0`: disconnected, `tcp_backend_send()` / `tcp_backend_recv()` will:

```c
// tcp_backend.c
if (ch->fd < 0) {
    ch->fd = try_reconnect(p, ch);    // exponential backoff
    if (ch->fd < 0) {
        snprintf(ch->errbuf, ..., "disconnected (reconnect in %dms)", ...);
        return -1;
    }
}
```

`try_reconnect()`:
1. Sleep for `recon_backoff` ms (100, 200, 400, ..., 5000 max)
2. Attempt `tcp_try_connect()`
3. On success: set `recon_backoff = 0`, return new fd
4. On failure: `recon_backoff *= 2`, cap at 5000, return -1

### 5.3 Server: multi-client poll loop

`recv_multi()` in `tcp_backend.c` is a dedicated poll loop:

```c
static int recv_multi(tcp_priv_t* p, xlink_channel_t* ch, void* buf, size_t* len) {
    for (;;) {
        // Build pollfd array: [listen_fd, client_0, client_1, ..., client_N]
        struct pollfd fds[MAX_CLIENTS + 1];
        nfds = 0;
        fds[nfds++] = { .fd = p->listen_fd, .events = POLLIN };
        for (int i = 0; i < p->nclients; i++)
            fds[nfds++] = { .fd = p->client_fds[i], .events = POLLIN };

        poll(fds, nfds, -1);

        // Accept new connections (exhaustive non-blocking accept)
        if (fds[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept(p->listen_fd, NULL, NULL);
                if (cfd < 0) break;   // EAGAIN → no more
                add_client(p, cfd);      // sets TCP_NODELAY, optional NONBLOCK
            }
        }

        // Read from ready clients
        for (int i = 1; i < nfds; i++) {
            if (!fds[i].revents) continue;
            // Read 4-byte header
            read_exact(client_fd, hdr, 4);
            // Read payload
            read_exact(client_fd, buf, msglen);
            return 0;                // return one message
        }
        // Loop back — poll again
    }
}
```

Key behaviors:
- **Infinite loop**: `recv_multi()` does NOT return -1 on "no data" — it blocks forever. This is a blocking call by design.
- **Swap-remove on disconnect**: `remove_client()` uses the `array[--n]` pattern (O(1) removal, no memmove).
- **Atomic framing**: `write_framed()` uses a single `writev(2)` with header+payload as two iovecs. Two separate `write()` calls could race on connection break, leaving the receiver with a 4-byte header and no payload, desyncing framing permanently.
- **Discard on oversized**: If a received message exceeds the caller's buffer, it reads and discards all bytes, then continues the poll loop. The caller gets ENOSPC and needs to retry with a larger buffer.

### 5.4 Server send: broadcast to all clients

```c
static int send_to_all(tcp_priv_t* p, void* data, size_t len, xlink_channel_t* ch) {
    int ok_count = 0;
    for (int i = p->nclients - 1; i >= 0; i--) {
        if (write_framed(p->client_fds[i], data, len) != 0) {
            close(p->client_fds[i]);
            p->client_fds[i] = p->client_fds[--p->nclients];   // swap-remove dead client
        } else {
            ok_count++;
        }
    }
    if (ok_count == 0) {
        snprintf(ch->errbuf, "tcp: no connected clients");
        return -1;
    }
    return 0;
}
```

- **Iterates backwards** because swap-remove at index `i` fills from `--nclients`, which is already past. Forward iteration would skip the client that moved into `i`.
- **writev(2) atomicity**: A single syscall writes both header and payload. On a stream socket, `writev` guarantees that the bytes from both buffers are written as a contiguous sequence (from the receiver's perspective). If the connection breaks mid-writev, the kernel writes whatever it has buffered — but since header and payload are in separate buffers, a partial write of the header only is possible if `writev` returns a value between 0 and 4. The write_framed() retry loop handles this correctly.

### 5.5 Dual `read_exact()` — one in core, one in TCP

xlink has two `read_exact()` implementations — same logic, slightly different features:

| Location | Feature |
|----------|---------|
| `src/xlink.c` (static) | Used by `frame_recv()`. No timeout protection. EAGAIN mid-read → poll(-1) retry. |
| `src/tcp_backend.c` (static) | Used by TCP framing. **30-second deadline** (Round 44 feature). EAGAIN mid-read → poll(up to deadline) retry. |

The 30s deadline in the TCP version prevents infinite hang on half-open TCP connections: if the peer dies mid-frame, `read_exact` won't wait forever.

### 5.6 NONBLOCK EAGAIN retry in send

`write_framed()` in `tcp_backend.c` retries on EAGAIN up to `MAX_WRITE_EAGAIN` (100) times, with 10ms POLLOUT waits between each retry (about 1 second total):

```c
if ((errno == EAGAIN || errno == EWOULDBLOCK)
    && eagain_retries < MAX_WRITE_EAGAIN) {
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    poll(&pfd, 1, 10);
    eagain_retries++;
    continue;
}
```

Without this, a non-blocking TCP socket with a full send buffer would immediately disconnect on EAGAIN — a rare but real failure mode.

---

## 6. `xlink_wait()` — Multi-Channel Polling

```c
int xlink_wait(xlink_channel_t** chans, int n, int timeout_ms);
```

Returns the index (0..n-1) of the first channel with data ready.

**Two strategies** based on whether all channels have real file descriptors:

### Strategy A: All-fd (`npfd == n`)

Single `poll()` call on all fds. Returns first ready channel.

```c
poll(pfds, npfd, timeout_ms);
for (int i = 0; i < npfd; i++)
    if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))
        return map[i];   // map: pollfd index → channel index
return -1;  // timeout
```

### Strategy B: Mixed or no-fd (`npfd < n`)

Periodic loop: short `poll()` on fds + `peek()` on SHM channels.

```c
for (;;) {
    // Poll fds with short timeout (max 100ms)
    poll(pfds, npfd, min(deadline_remain, 100));
    // Check fds for data
    for (int i = 0; i < npfd; i++)
        if (pfds[i].revents) return map[i];

    // Peek SHM channels
    for (int i = 0; i < n; i++)
        if (chans[i]->fd < 0 && chans[i]->backend->peek) {
            avail = chans[i]->backend->peek(...);
            if (avail > 0) return i;
        }

    // No pollable fds at all → 5ms sleep
    if (npfd == 0) usleep(5000);

    // Deadline check happens AFTER at least one poll+peek cycle
    if (timeout_ms >= 0 && current_ms() >= deadline_ms)
        return -1;
}
```

The deadline check is intentionally AFTER the first poll+peek, so that `timeout_ms = 0` still does one iteration.

**For SHM-only channels** (npfd=0, has_peek=true): polls with `usleep(5000)` + peek in a tight loop. Tested in `tests/test_wait_edge.c` with `test_pure_shm_infinite_wait()` (20 checks, Round 41).

**Edge case**: If npfd=0 and no backend provides peek (`has_peek == false`), the function returns `-2 / ENOTSUP` immediately.

---

## 7. Framing Layer Details

### 7.1 Frame format

```
+------------------+------------------------+
|  4 bytes (BE)    |   N bytes              |
|  payload length  |   raw payload          |
+------------------+------------------------+
```

`frame_send()`, `src/xlink.c`:
```c
uint8_t header[4];
write_u32_be(header, (uint32_t)len);
struct iovec iov[2] = {
    { .iov_base = header, .iov_len = 4 },
    { .iov_base = (void*)data, .iov_len = len },
};
// writev in a retry loop, advancing iov pointers on partial writes
```

`frame_recv()`:
```c
read_exact(ch->fd, header, 4);        // 4-byte BE length
uint32_t payload_len = read_u32_be(header);
if (payload_len > *len) {
    // Discard: read & drop all payload bytes to maintain framing sync
    while (remaining > 0) {
        read_exact(ch->fd, discard, min(remaining, 4096));
        remaining -= n;
    }
    return -1 (ENOSPC);  // caller retries with larger buffer
}
read_exact(ch->fd, buf, payload_len);
*len = payload_len;
return 0;
```

### 7.2 The discard design

When the caller's buffer is too small, the framer **discards** the message rather than returning a partial or breaking sync. This is intentional:

1. **Partial return** would leave the next `recv()` call reading from the middle of a frame — total desync.
2. **Connection close** is too aggressive for a library.
3. **Discard + continue** keeps the stream perfectly synchronized at the cost of one lost message.

The caller gets `errno == ENOSPC` and can retry with a larger buffer. Documented in `design-decisions.md` #1.

---

## 8. SHM Backend — Wrapping an External Library

The SHM backend is the simplest. It wraps `shm_ipc`, the existing name-based shared memory library:

```c
// shm_backend.c
typedef struct {
    char name[64];
} shm_priv_t;

static int shm_backend_open(ch, addr, opt) {
    if (flags & XLINK_CREATE)
        shm_create(name);             // or shm_create_broadcast(name, 16)
    else
        ;  // passive: segment must already exist
    xlink_register_shm_cleanup(name);  // atexit cleanup
}

static int shm_backend_send(ch, data, len) {
    shm_writen(p->name, data, len);
}

static int shm_backend_recv(ch, buf, *len) {
    if (flags & XLINK_NONBLOCK)
        shm_read(p->name, buf, len);   // non-blocking
    else
        shm_readn(p->name, buf, len);  // blocking
}
```

**atexit cleanup** (`xlink_register_shm_cleanup`): Each `XLINK_CREATE | XLINK_SHM` open registers the name in a fixed-size array (`MAX_CLEANUP = 256`). At process exit (or `exit()` call), all registered SHM segments are destroyed. This handles the case where a process terminates normally but forgets to call `shm_destroy()`.

Not handled: `SIGKILL` vs `SIGTERM`. `atexit` doesn't run on `SIGKILL` (`kill -9`). Users should have a cleanup strategy for that case (e.g., `ipcrm -M / shm_unlink`).

---

## 9. UDP Backend — Unicast + Multicast

A simpler message-based backend. The vtable has `.read = NULL`, `.write = NULL`, `.peek = NULL`:

```c
const xlink_backend_t xlink_udp_backend = {
    .open  = udp_backend_open,
    .close = udp_backend_close,
    .send  = udp_backend_send,    // sendto()
    .recv  = udp_backend_recv,    // recvfrom()
    .write = NULL,
    .read  = NULL,
    .peek  = NULL,
};
```

**Send path**:
- If `dest_len > 0` (unicast address configured): `sendto(fd, data, len, 0, dest_addr, dest_len)`
- If `dest_len == 0` (receiver mode, no dest): falls through to `write(fd, data, len)` on the unconnected socket — this fails with `EDESTADDRREQ`. This is by design: a receiver-channel shouldn't be used for sending. Tested in Round 40.

**Multicast** (`setsockopt(IP_ADD_MEMBERSHIP)`): if the address is a multicast group (224.0.0.0/4), `setsockopt(IP_ADD_MEMBERSHIP, ...)` is called in open. Destination is the group address, so `sendto()` sends to the group.

---

## 10. Serial Backend — Termios Configuration

Stream backend with framing. Opens the device with `O_RDWR` (CREATE) or `O_RDONLY`.

Termios config:
- **Data bits**: 8 (default), 7 or 5 via `opt->serial.bits`
- **Parity**: None (default), Even, Odd via `opt->serial.parity`
- **Stop bits**: 1 (default), 2 via `opt->serial.stop`
- **Flow control**: Disabled (`~CRTSCTS`)
- **VMIN=1, VTIME=0**: `read()` blocks until at least 1 byte available

Baud rate lookup table (`baud_to_speed()`): known rates → B4800...B230400. Unknown rates → B9600 fallback (design decision #2).

---

## 11. Pipe Backend — POSIX FIFO

Simple stream backend. Opens FIFO with `O_RDWR` (avoids the classic FIFO blocking-open problem — opening read-only blocks until a writer opens, and vice versa).

```c
// pipe_backend.c (core logic)
if (flags & XLINK_CREATE)
    mkfifo(path, 0666);     // create if not exists

ch->fd = open(path, O_RDWR);    // non-blocking open
```

---

## 12. File Backend — Dump / Replay

The file backend reads and writes files containing `[4-byte BE length][payload]` sequences — the same format as the wire framing. This allows recording and replaying communication sessions.

- `XLINK_CREATE`: open for writing (truncate or append)
- No CREATE: open for reading
- Recv reads one record (4-byte length + payload)
- Send appends one record (4-byte length + payload)

Used for offline analysis of communication patterns.

---

## 13. Bridge Tool (`tools/bridge.c`)

The bridge is the "killer app" — transparent forwarding between any two transports without writing code:

```
xlink-bridge shm://sensor_data tcp://:9000
xlink-bridge --bidir serial:///dev/ttyUSB0:115200 udp://224.1.1.1:5555
```

**Unidirectional** (`forward()`): simple loop — `xlink_recv(A)` → `xlink_send(B)`.

**Bidirectional** (`forward_bidir()`): uses `xlink_wait()` to poll both channels:

```c
xlink_channel_t* chans[2] = { chA, chB };

while (1) {
    int idx = xlink_wait(chans, 2, -1);  // wait forever
    src = chans[idx];
    dst = chans[1 - idx];
    xlink_recv(src, buf, &len);
    xlink_send(dst, buf, len);
}
```

The bridge is the intended production use case: connect an embedded device over serial to a cloud server over TCP, with xlink as the middleware.

---

## 14. Testing Strategy

30 test binaries (auto-discovered via Makefile wildcard `$(wildcard tests/test_*.c)`):

| Test file | Coverage |
|-----------|----------|
| `test_basic.c` | Basic send/recv on all 6 backends |
| `test_errors.c` | Error paths: open failure, send to unopened, etc. |
| `test_frame_overflow.c` | Oversized message discard |
| `test_tcp_overflow_client.c` | TCP oversized recv |
| `test_tcp_errbuf.c` | TCP disconnect error messages |
| `test_serial_edge.c` | Serial open failure, baud rate |
| `test_udp_edge.c` | UDP receiver mode, send failure |
| `test_wait_edge.c` | `xlink_wait()` edge cases: pure SHM, mixed SHM+pipe |
| `test_pipe_edge.c` | Pipe: write-only, read-only, multi-writer |
| `stress_shm.c` | High-throughput SHM stress test |
| `stress_tcp.c` | High-throughput TCP stress test |

**Build verification**:
```sh
make clean && make all    # 0 warnings with -Wall -Wextra -O2 -g
make test                 # ~300+ checks, ALL PASS (~3 seconds)
```

---

## 15. Adding a New Backend

To add a new transport (e.g., a Bluetooth backend):

**Step 1:** Create `src/bt_backend.c` implementing the `xlink_backend_t` vtable.

```c
const xlink_backend_t xlink_bt_backend = {
    .type = XLINK_BLUETOOTH,  // add to xlink_type_t enum
    .name = "bt",
    .open = bt_backend_open,  // allocate priv, set ch->fd
    .close = bt_backend_close,// close fd, free priv
    .send = bt_backend_send,  // write data to bt socket
    .recv = bt_backend_recv,  // read data from bt socket
    .write = NULL,            // use send as fallback
    .read = NULL,             // use recv as fallback (ignoring timeout)
    .peek = NULL,             // no peek support
};
```

**Step 2:** Register in `backends[]` in `src/xlink.c` and add `XLINK_BLUETOOTH` to the `use_framing` check if it's a stream transport.

**Step 3:** Add the `.c` file to Makefile's `OBJS` list.

Total: ~3-4 changes, ~100-150 lines.

---

## 16. v2.0: Plugin Architecture (`src/plugin.c`)

Added 2026-05-29. Enables dynamic backend loading via `.so` files (dlopen).

### 16.1 Plugin Registry

A hash table (32 buckets, djb2 hash, chained linked lists) stores all
registered plugins. Protected by `pthread_mutex_t` for thread safety.

```
plugin_table[32]
  bucket[0] → plugin_entry("shm")  → NULL
  bucket[5] → plugin_entry("tcp")  → plugin_entry("pipe") → NULL
  ...
```

### 16.2 Key Functions

| Function | Role |
|----------|------|
| `xlink_plugin_register()` | Add a built-in or loaded plugin to the registry |
| `xlink_plugin_find(name)` | Lookup by string name (djb2 hash → chain walk) |
| `xlink_plugin_find_by_type(type)` | Lookup by protocol type ID (linear scan) |
| `xlink_plugin_load(so_path)` | dlopen a `.so`, dlsym `xlink_plugin_export`, register it |
| `xlink_plugin_count()` | Return number of registered plugins |
| `xlink_open_url(url, opt)` | Parse `scheme://path` → find plugin → call `plugin->open()` |

### 16.3 .so Plugin Contract

Plugins must export exactly one symbol:
```c
const xlink_plugin_t xlink_plugin_export;
```

The `xlink_plugin_t` struct includes `api_version` (currently 1) for ABI
compatibility checking. If the plugin's version doesn't match xlink's
`XLINK_PLUGIN_API_VERSION`, loading is refused.

Dynamic plugin protocol type IDs start at `XLINK_USER_BASE = 16`, keeping
0–6 reserved for built-in backends.

### 16.4 Built-in Backend Registration

On startup, `init_plugins()` in `src/xlink.c` registers all 6 built-in
backends (SHM/PIPE/TCP/UDP/SERIAL/FILE) as plugins via
`xlink_plugin_register()`. This means `xlink_open(type, ...)` and
`xlink_open_url("shm://...", ...)` go through the same code path.

---

## 17. v2.0: Async I/O Engine (`src/aio*.c`)

Added 2026-05-29. Replaces polling `xlink_wait()` with event-driven I/O.

### 17.1 Engine Architecture

```
xlink_aio_create(type) → engine instance (opaque void*)
  │
  ├── type=AUTO(0) → try epoll_create1() → success → epoll engine
  │                  → failure → poll engine (POSIX fallback)
  ├── type=EPOLL(2) → epoll engine only
  ├── type=POLL(1)  → poll engine only
  └── type=IO_URING(3) → reserved (future)
```

### 17.2 Engine vtable (`src/aio.h` — internal)

```c
typedef struct xlink_aio_ops {
    int  (*watch)(xlink_aio_t *aio, int fd, void *ch);
    int  (*unwatch)(xlink_aio_t *aio, int fd);
    int  (*wait)(xlink_aio_t *aio, int timeout_ms,
                 int *out_fd, void **out_user);
    void (*destroy)(xlink_aio_t *aio);
} xlink_aio_ops_t;
```

### 17.3 epoll Engine (`src/aio_epoll.c`)

- `watch()`: `epoll_ctl(EPOLL_CTL_ADD)` with `EPOLLIN`
- `unwatch()`: `epoll_ctl(EPOLL_CTL_DEL)`
- `wait()`: `epoll_wait()` → return one ready fd + user data pointer
- Thread-safe with `pthread_rwlock_t` on the fd→channel map

### 17.4 poll Engine (`src/aio_poll.c`)

POSIX fallback. Maintains a dynamic `struct pollfd[]` array. `watch()` adds
fd + events, `unwatch()` compacts the array, `wait()` calls `poll()`.

### 17.5 Public API Integration

```c
// Create engine (0=AUTO, 1=POLL, 2=EPOLL)
void *xlink_aio_create(int type);
void  xlink_aio_destroy(void *engine);

// Event-driven alternative to xlink_wait()
int xlink_wait_aio(xlink_channel_t **chans, int n,
                   int timeout_ms, void *aio_engine);
```

`xlink_wait_aio()` internally:
1. Registers all channel fds with the engine via `watch()`
2. Calls `engine->wait(timeout_ms)` to block until data arrives
3. Returns the ready channel index (same semantics as `xlink_wait()`)
4. For SHM channels (no fd), falls back to a short `usleep(5000)` + `peek()`
   loop — this is the last remaining polling path. Future versions will
   replace it with eventfd (step 2.5 in `02-async-io-phases.md`).

### 17.6 Remaining Steps

| Step | Status | Description |
|------|--------|-------------|
| 2.5 SHM eventfd | ✅ v2.1 | Replace shm peek polling with eventfd notification |
| 2.6 xlink_run() | ✅ v2.1 | Event-driven main loop with callback |
| 2.7 io_uring | ✅ v2.1 | Kernel-level async I/O (Linux 5.1+) |
| 2.9 Docs | ✅ v2.1 | This file updated |

---

## 18. v2.1: Async I/O Deepening

### 18.1 SHM eventfd Wake-up (`src/shm_backend.c`)

v2.0 used `usleep(5000)` + `peek()` polling for SHM channels in
`xlink_wait_aio()` — the last remaining polling path. v2.1 replaces this
with an eventfd-based notification:

- `shm_backend.c` creates a per-channel `eventfd(0, EFD_NONBLOCK)` during
  `open()` and stores the fd in the channel's private data.
- On `send()`, the backend writes 8 bytes to the eventfd to signal the
  receiver.
- `xlink_wait_aio()` registers the eventfd in epoll alongside channel fds.
  On wake-up, it drains the eventfd (reads 8 bytes) and peeks SHM to verify
  data availability.
- `close()` releases the eventfd.

This eliminates the 5ms polling latency entirely — SHM delivery is now
truly event-driven with sub-millisecond wake-up.

### 18.2 xlink_run() Event Loop (`src/aio.c`)

```c
int xlink_run(xlink_channel_t **chans, int n,
              void *aio_engine,
              void (*callback)(int idx, void *arg),
              void *arg, int timeout_ms);
```

A production-ready event loop that replaces the manual `while(1) { wait;
process }` pattern:

1. If `aio_engine` is NULL, auto-creates an epoll engine (and destroys it
   on exit).
2. Iterates `wait_aio()` → calls `callback(idx, arg)` for each ready
   channel.
3. Returns normally on callback returning non-zero, or -1 on timeout/error.
4. Stale detection: if `wait_aio()` returns an index but SHM peek shows no
   data (spurious eventfd wake-up), the loop skips the callback and
   continues.
5. 24 checks in `test_run.c` covering single/multi-event, timeout, auto-
   engine creation, and error cases.

### 18.3 io_uring Engine (`src/aio_uring.c`)

Linux 5.1+ kernel-level async I/O via raw syscall interface — zero external
dependencies (no `liburing`).

```c
#define XLINK_AIO_IOURING 3  // in xlink.h
```

**Architecture:**
- `aio_uring_create()`: calls `io_uring_setup(256, &params)` directly via
  `syscall(__NR_io_uring_setup, ...)`.
- Submission ring (SQ) and completion ring (CQ) mapped via `mmap()`.
- `watch(fd)`: adds the fd to an internal array; the actual SQE submission
  happens in `wait()`.
- `wait()`: submits `IORING_OP_READ` SQEs for all watched fds, then calls
  `io_uring_enter(min_complete=1, ...)` to block until at least one CQE
  arrives. Returns the index of the ready fd.
- `destroy()`: unmaps SQ/CQ rings and closes the ring fd.

**Engine registration** in `aio.c`:
- `xlink_aio_create(XLINK_AIO_IOURING)` → creates the uring engine.
- `xlink_aio_create(AUTO)` still prefers epoll (io_uring requires Linux
  5.1+, which is not yet universal).

**Key design decisions:**
- Raw syscall interface keeps the library zero-dependency.
- SQE submission is batched in `wait()` — we don't submit on every `watch()`
  because the poll loop pattern is "watch many, wait once".
- The engine supports up to 256 concurrent fds (configurable via
  `IO_URING_ENTRIES` define).

### 18.4 Integration Summary

| Component | Source | Public API | Tests |
|-----------|--------|------------|-------|
| SHM eventfd | `src/shm_backend.c` | internal | `test_aio.c` (35 checks) |
| xlink_run() | `src/aio.c` | `xlink_run()` | `test_run.c` (24 checks) |
| io_uring | `src/aio_uring.c` | `xlink_aio_create(3)` | `test_aio.c` (compat) |

---

## 19. Document Cross-Reference

| Document | What it covers | Read when |
|----------|---------------|-----------|
| `api.md` | Public API reference: every function, flag, option struct | Writing code against xlink |
| `integration-guide.md` | Third-party module integration via libxlink or wire protocol | Adding a new consumer |
| `proposal.md` | Original design, architecture overview, status per component | Historical context |
| `design-decisions.md` | 8 intentional design choices with rationale | Questioning "why" |
| `known-issues.md` | 4 remaining known issues (3-6), 5 fixed (1/2/7/8/9) | Pre-deployment review |
| `slab-allocator.md` | Proposed slab allocator for channel_t + priv (draft) | Performance tuning |
| `future-plans/index.md` | Roadmap: P0/P1/P2, dependency graph, decision log | Strategic planning |
| `future-plans/01-plugins-arch.md` | Dynamic backend loading via dlopen | Next gen architecture |
| `future-plans/02-async-io.md` | io_uring / epoll-based async I/O | Performance roadmap |
| `future-plans/03-tls-security.md` | TLS encryption via OpenSSL/WolfSSL | Security roadmap |
| `next-version-thoughts.md` | Historical changelog (Rounds 40-45) | Recent changes |

---

## Appendix: `xlink_send()` Full Call Chain

```
                xlink_send(ch, data, len)
                        │
         ┌──────────────┴──────────────┐
         │ use_framing == true?         │
         └───────┬─────────────┬───────┘
                 │             │
            (yes)│             │(no)
                 ▼             ▼
          frame_send()    ch->backend->send()
               │                 │
      writev(fd, [hdr,     ┌─────┴─────┐
      payload], 2)         │           │
                    tcp: write_framed() udp: sendto()
                    shm: shm_writen()  file: write()
                    pipe: writev()     serial: write()
```

## Appendix: `xlink_recv()` Full Call Chain

```
                xlink_recv(ch, buf, len)
                        │
         ┌──────────────┴──────────────┐
         │ use_framing == true?         │
         └───────┬─────────────┬───────┘
                 │             │
            (yes)│             │(no)
                 ▼             ▼
          frame_recv()    ch->backend->recv()
               │                 │
      read_exact(4→hdr)    ┌─────┴─────┐
      oversized? → discard │           │
      read_exact(payload)  │     shm: shm_readn()
                           │     udp: recvfrom()
                    tcp: read_framed()  file: read()
                    pipe: read()        serial: read()
                    (tcp with reconnect)
```

---

*End of code walkthrough. ~2,500 lines of reference source, ~500 lines of documentation.*

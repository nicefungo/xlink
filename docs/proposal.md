# xlink — Cross-App Communication Toolkit

> **实现状态：** 核心库已实现。6 个后端全部完成（SHM, PIPE, TCP, UDP, SERIAL, FILE）。
> CLI 工具源码存在但尚未加入构建系统。RTSP 端口的枚举已预留，后端未实现。
> 详见下方各章节的 ✅/⏳ 标记。

## 项目目标

为 C/C++ 工程师提供一套**统一 API 的跨应用/跨设备通信工具箱**，覆盖：

- **同机多进程**：共享内存、命名管道（已有 `shm_ipc`）
- **同网多设备**：TCP、UDP、组播
- **硬件直连**：串口（RS-232/485）
- **音视频流**：RTSP 拉流
- **协议桥接**：在不同的传输之间透明转发

**核心理念**：所有传输用同一个 API，换底层不换代码。

---

## 1. 架构总览

```
                    ┌──────────────────────────┐
                    │     Application Code      │
                    │   send/recv data via fd   │
                    └────────────┬─────────────┘
                                 │
                    ┌────────────▼─────────────┐
                    │      xlink API            │
                    │  xlink_open/send/recv     │
                    └────────────┬─────────────┘
                                 │
         ┌───────────────────────┼───────────────────────┐
         │                       │                       │
    ┌────▼────┐           ┌──────▼──────┐          ┌─────▼─────┐
    │  SHM    │           │   TCP/UDP   │          │   Serial   │
    │ Backend │           │   Backend   │          │   Backend  │
    └─────────┘           └─────────────┘          └───────────┘
         │                       │                       │
    shm_open + mmap        socket() + connect()      open() + tcsetattr()

         ┌───────────────────────┼───────────────────────┐
         │                       │                       │
    ┌────▼────┐           ┌──────▼──────┐          ┌─────▼─────┐
    │  Pipe   │           │    RTSP     │          │   Bridge   │
    │ Backend │           │   Backend   │          │   Daemon   │
    └─────────┘           └─────────────┘          └───────────┘
```

### 分层职责

| 层 | 责任 |
|----|------|
| **xlink.h** | 公共 API：open / send / recv / close |
| **Transport Backend** | 封装具体系统调用，提供统一 `xlink_ops` vtable |
| **Framing Layer** | 为流式传输（TCP/串口）添加消息定界 |
| **Bridge** | 在任意两个 transport 之间透明转发 |
| **CLI Tools** | `xlink-send`, `xlink-recv`, `xlink-bridge`, `xlink-monitor` |

---

## 2. 公共 API 设计（xlink.h）

```c
#ifndef XLINK_H
#define XLINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Transport types ──────────────────────────────────────── */
typedef enum {
    XLINK_SHM,     /* POSIX shared memory (your shm_ipc) */
    XLINK_PIPE,    /* Named pipe (FIFO) */
    XLINK_TCP,     /* TCP client or server */
    XLINK_UDP,     /* UDP unicast / multicast */
    XLINK_SERIAL,  /* RS-232 / RS-485 */
    XLINK_RTSP,    /* RTSP client (pull video stream) */
    XLINK_FILE,    /* File I/O (read from / write to file) */
} xlink_type_t;

/* ── Open flags ───────────────────────────────────────────── */
typedef enum {
    XLINK_CREATE  = 1 << 0,  /* Create if not exists (SHM)   */
    XLINK_SERVER  = 1 << 1,  /* Bind as server (TCP, PIPE)   */
    XLINK_NONBLOCK = 1 << 2, /* Non-blocking I/O             */
    XLINK_BROADCAST = 1 << 3,/* One-to-many (SHM broadcast)  */
} xlink_flag_t;

/* ── Options (per transport) ──────────────────────────────── */
typedef struct {
    uint32_t     flags;          /* bitmask of xlink_flag_t   */
    size_t       buf_size;       /* buffer / shm size         */
    int          timeout_ms;     /* recv timeout, -1 = block  */
    union {
        struct { int mode;    } shm;     /* compete / broadcast */
        struct { int backlog; } tcp;     /* listen() backlog    */
        struct { int baud;    } serial;  /* e.g. 115200         */
        struct { const char* iface; int ttl; } mcast;
    };
} xlink_opt_t;

/* ── Handle (opaque) ─────────────────────────────────────── */
typedef struct xlink_channel xlink_channel_t;

/* ── Core API ─────────────────────────────────────────────── */

/* Open a channel. addr format depends on type:
 *   XLINK_SHM    → "/my_shm"              (shm name)
 *   XLINK_PIPE   → "/tmp/my_pipe"         (FIFO path)
 *   XLINK_TCP    → "192.168.1.5:8080"     (client)
 *                → ":8080"                (server)
 *   XLINK_UDP    → "224.1.1.1:5555"       (multicast group)
 *   XLINK_SERIAL → "/dev/ttyUSB0:115200"  (device:baud)
 *   XLINK_RTSP   → "rtsp://camera.local/stream1"
 */
xlink_channel_t* xlink_open(xlink_type_t type, const char* addr,
                            const xlink_opt_t* opt);

/* Send a framed message (length + payload). */
int xlink_send(xlink_channel_t* ch, const void* data, size_t len);

/* Receive a framed message. *len is capacity in, actual size out.
 * Returns 0 on success, -1 on error/timeout. */
int xlink_recv(xlink_channel_t* ch, void* buf, size_t* len);

/* Low-level stream read/write (for custom framing). */
int xlink_write(xlink_channel_t* ch, const void* data, size_t len);
int xlink_read(xlink_channel_t* ch, void* buf, size_t len, int timeout_ms);

/* Peek at available data without consuming (if transport supports). */
int xlink_peek(xlink_channel_t* ch, size_t* avail);

/* Close and free. */
void xlink_close(xlink_channel_t* ch);

/* Return string describing last error on this channel. */
const char* xlink_errstr(xlink_channel_t* ch);

/* ── Utility ──────────────────────────────────────────────── */

/* Pretty-print transport info for debugging. */
void xlink_dump(xlink_channel_t* ch, int fd);

/* Convert type enum to string. */
const char* xlink_type_str(xlink_type_t t);

#ifdef __cplusplus
}
#endif

#endif /* XLINK_H */
```

### 关键设计决策

**为什么用 `int` 返回值而不是 errno？**
- 保持与 POSIX 习惯一致：负值表示错误
- `xlink_errstr()` 提供人类可读的错误描述
- 每个 channel 内部记录上次错误码

**为什么 send/recv 基于"消息"而非流？**
- SHM 本质是消息传递（写一块→读一块）
- TCP/串口是流，但绝大多数应用场景需要消息边界
- 内部 Framing Layer 自动为流式传输添加 4 字节长度前缀
- 用户如果不想要自动分帧，可以用 `xlink_write/read`

---

## 3. 内部架构

### 3.1 Transport Backend 模式

每个后端实现一个 vtable：

```c
/* Internal — not exposed in public header */
typedef struct {
    xlink_type_t type;
    const char*  name;       /* "shm", "tcp", "serial"... */

    /* Open / Close */
    int  (*open)(xlink_channel_t* ch, const char* addr, const xlink_opt_t* opt);
    void (*close)(xlink_channel_t* ch);

    /* Messaging API */
    int  (*send)(xlink_channel_t* ch, const void* data, size_t len);
    int  (*recv)(xlink_channel_t* ch, void* buf, size_t* len);
    int  (*peek)(xlink_channel_t* ch, size_t* avail);

    /* Stream API (optional — fallback to send/recv framing) */
    int  (*write)(xlink_channel_t* ch, const void* data, size_t len);
    int  (*read)(xlink_channel_t* ch, void* buf, size_t len, int timeout_ms);
} xlink_ops_t;
```

channel 结构体：

```c
struct xlink_channel {
    const xlink_ops_t* ops;    /* vtable */
    int                fd;     /* generic fd, or -1 for SHM */
    int                flags;  /* open flags */
    int                err;    /* last error code */
    void*              priv;   /* backend-specific state */
    /* Framing layer (for stream transports) */
    uint8_t*           rbuf;
    size_t             rcap;
    size_t             rlen;   /* buffered bytes */
};
```

### 3.2 帧格式（流式传输用）

```
┌──────────┬──────────────────────────┐
│   Len    │         Payload          │
│ (4 bytes,│       (Len bytes)        │
│  big-end)│                          │
└──────────┴──────────────────────────┘
```

对于 TCP/串口：
- `xlink_send` 自动在前面加上 4 字节长度
- `xlink_recv` 先读 4 字节得到长度，再读那么多字节
- 内部有接收缓冲，应对粘包/半包

对于 SHM/UDP（已是消息边界）：
- 长度前缀 `xlink_send` 不会加，"你写啥就是啥"
- 但如果收到超出 `buf` 大小的消息，返回 `-1` + 错误码

### 3.3 后端实现概述

| 后端 | 核心系统调用 | 特殊处理 |
|------|-------------|---------|
| **SHM** | `shm_open`, `mmap`, `pthread_mutex_lock` | 复用已有 `shm_ipc` 代码；支持 compete / broadcast 模式 |
| **Pipe** | `mkfifo`, `open`, `read`, `write` | Server 端用 `XLINK_SERVER` flag |
| **TCP** | `socket`, `bind`, `listen`, `accept`, `connect` | 非阻塞 + poll；自动重连选项 |
| **UDP** | `socket`, `sendto`, `recvfrom`, `setsockopt` | 支持组播（IGMP join） |
| **Serial** | `open`, `tcsetattr`, `read`, `write` | baud rate / parity / stop bits 可配置 |
| **RTSP** | 通过 libcurl 或自己实现 RTSP over TCP | 解析 SDP → 拉 RTP/UDP 流 → 回调 |
| **File** | `open`, `read`, `write` | 用于录制/回放测试数据 |

---

## 4. 桥接模式（最重要特性）

桥接是让任何两个 transport 之间透明转发。这是该工具箱与原始 socket API 的**核心价值差异**。

```
  ┌─────────┐    TCP      ┌───────────┐    SHM      ┌─────────┐
  │ Sensor  │◄───────────►│ xlink-bridge│◄──────────►│  AI      │
  │ (TCP)   │  :8080      │  daemon    │  /shm_ai   │  Process │
  └─────────┘             └───────────┘             └─────────┘
```

CLI 使用示例：

```sh
# 把 SHM 上收发的数据转发到 TCP 端口
xlink-bridge shm://my_shm tcp://:9090

# 把串口数据桥接到 UDP 组播
xlink-bridge serial:///dev/ttyUSB0:115200 udp://224.1.1.1:5555

# 双向桥接（A 读到 B 写，B 读到 A 写）
xlink-bridge --bidir shm://shm1 tcp://192.168.1.5:8080

# 录制 SHM 数据到文件（回放用）
xlink-bridge shm://my_shm file:///tmp/capture.bin
```

内部实现就是两个 `xlink_channel_t` + 双线程或 `poll()` 事件循环：

```c
/* Pseudocode: xlink-bridge main loop */
void bridge_loop(xlink_channel_t* a, xlink_channel_t* b, int bidir) {
    uint8_t buf[65536];
    while (running) {
        size_t len = sizeof(buf);
        if (xlink_recv(a, buf, &len) == 0)
            xlink_send(b, buf, len);
        if (bidir) {
            len = sizeof(buf);
            if (xlink_recv(b, buf, &len) == 0)
                xlink_send(a, buf, len);
        }
    }
}
```

---

## 5. CLI 工具

> 源码均已实现（`tools/` 目录），但未加入构建系统。如需使用，可手动编译。

| 工具 | 源文件 | 功能 |
|------|--------|------|
| `xlink-send` | `tools/send.c` | 从 stdin 读取并发送到 channel |
| `xlink-recv` | `tools/recv.c` | 从 channel 接收并写入 stdout |
| `xlink-bridge` | `tools/bridge.c` | 在两个 channel 之间转发 |
| `xlink-monitor` | `tools/monitor.c` | 监听 channel 并打印 hex dump |

这些工具让**无需写代码就能连接不同模块**——shell 脚本也能用。

---

## 6. 实现路线图

### Phase 1 — 核心框架 + SHM + Pipe ✅

```
xlink.h / xlink.c        # API 调度层、错误处理、framing 层
shm_backend.c             # 封装 shm_ipc 作为后端
pipe_backend.c            # 命名管道
```

交付物：SHM 和 PIPE 通道可通过统一 API 收发消息。

### Phase 2 — 网络传输 + 桥接 ✅

```
tcp_backend.c             # TCP client + server
udp_backend.c             # UDP unicast + multicast
```

交付物：TCP 和 UDP 通道可通过统一 API 收发消息。桥接源码存在（`tools/bridge.c`）但未加入构建系统。

### Phase 3 — 串口 + 文件 ✅

```
serial_backend.c          # RS-232/485
file_backend.c            # dump / replay
```

交付物：串口和 File I/O 通道可通过统一 API 收发消息。

### Phase 4 — RTSP + 稳定性 ⏳（未实现）

```
rtsp_backend.c            # RTSP over TCP → RTP depay（枚举已预留，实现待定）
```

交付物：RTSP 后端尚未实现。`XLINK_RTSP` 枚举已预留。

---

## 7. 与已有 `shm_ipc` 的关系

`shm_ipc` 保持独立，作为 `xlink` 的 SHM 后端：

```
shm_ipc/
├── include/shm_ipc.h     ← xlink 的 shm_backend.c 依赖这个
├── src/shm_ipc.c
├── bin/libshm_ipc.a      ← xlink 静态链接这个
└── ...                    ← 其余不变
```

**不需要改 shm_ipc 的 API。** SHM 后端只是把 `xlink_send` 映射到 `shm_write`，把 `xlink_recv` 映射到 `shm_read`。

---

## 8. 不需要的外部依赖

目标是**零外部依赖**，只用 POSIX 标准 + Linux 下的标准系统库。

| 组件 | 依赖 |
|------|------|
| 所有传输层 | 仅 `-lpthread -lrt`（可能加 `-lm`） |
| RTSP | 内置最小 RTSP 解析器（~300 行），或可选链接 libcurl |
| 序列化 | 用户自己负责 —— 我们只传 raw bytes |

---

## 9. 设计权衡记录

| 决策 | 选型 | 放弃方案 | 理由 |
|------|------|---------|------|
| API 风格 | C（过程式） | C++ class | C 更易嵌入各种项目；C 编译器远多于 C++ |
| 消息格式 | 4 字节长度前缀 | JSON / protobuf | 最小开销；上层可以根据需要加序列化 |
| 线程模型 | 用户控制 | 内部线程池 | 用户最清楚自己的并发模型 |
| 配置方式 | 程序化 + CLI | YAML / toml | 不用引入解析器 |
| 桥接实现 | poll() 双事件循环 | select / epoll | 可移植性优先 |
| 构建系统 | Makefile | cmake / meson | 保持与 shm_ipc 一致 |

---

## 10. 快速验证（通过测试）

当前验证方式直接运行测试套件：

```sh
make clean && make all && make test
```

测试覆盖了所有 6 个后端的基础收发、边界条件、错误路径。测试源码在 `tests/` 目录，`Makefile` 使用 wildcard 自动发现新测试。

---

## 附录：当前实现状态表

| 组件 | 状态 | 备注 |
|------|------|------|
| Core API (`xlink.h` / `xlink.c`) | ✅ 完成 | vtable 分发、framing、错误处理 |
| SHM Backend | ✅ 完成 | 复用 shm_ipc |
| PIPE Backend | ✅ 完成 | 命名管道 |
| TCP Backend | ✅ 完成 | Client + Server，自动重连 |
| UDP Backend | ✅ 完成 | Unicast + Multicast |
| SERIAL Backend | ✅ 完成 | RS-232/485 |
| FILE Backend | ✅ 完成 | 录制/回放 |
| RTSP Backend | ⏳ 未实现 | 枚举已预留 |
| CLI Tools | ⏳ 源码就绪 | 4 个工具（send/recv/bridge/monitor），未加入构建系统 |
| Slab Allocator | 📝 设计讨论 | 见 `docs/slab-allocator.md` |
| 插件化架构 | 📝 规划中 | 见 `docs/future-plans/01-plugins-arch.md` |
| 异步 I/O | 📝 规划中 | 见 `docs/future-plans/02-async-io.md` |
| TLS 加密 | 📝 规划中 | 见 `docs/future-plans/03-tls-security.md` |

# xlink API Reference

## 概览

```c
#include "xlink.h"
```

所有操作通过一个不透明指针 `xlink_channel_t*` 完成：

1. `xlink_open()`  — 打开通道
2. `xlink_send()`  — 发送消息
3. `xlink_recv()`  — 接收消息
4. `xlink_close()` — 关闭通道

---

## xlink_open

```c
xlink_channel_t* xlink_open(xlink_type_t type, const char* addr,
                            const xlink_opt_t* opt);
```

打开一个通信通道。

| 参数 | 说明 |
|------|------|
| `type` | 传输类型（见下方） |
| `addr` | 地址字符串，格式依类型而异 |
| `opt`  | 选项（可 NULL），见 `XLINK_OPT_DEFAULT` |

**地址格式：**

| 类型 | 地址格式 | 示例 |
|------|---------|------|
| `XLINK_SHM` | `name` — POSIX 共享内存名 | `"/my_shm"` |
| `XLINK_PIPE` | `/path/to/fifo` — FIFO 路径 | `"/tmp/xlink.pipe"` |
| `XLINK_TCP` | `host:port` (client) 或 `:port` (server) | `"192.168.1.5:8080"` / `":8080"` |
| `XLINK_UDP` | `host:port` (unicast) 或 `group:port` (multicast) | `"224.1.1.1:5555"` |
| `XLINK_SERIAL` | `/dev/ttyX:baud` | `"/dev/ttyUSB0:115200"` |
| `XLINK_RTSP` | `rtsp://...` URL | `"rtsp://camera/stream1"` |
| `XLINK_FILE` | `/path/to/file` | `"/tmp/capture.bin"` |

**返回：** 成功返回通道指针，失败返回 NULL（用 `xlink_errstr()` 或 `errno` 查原因）。

---

## xlink_send

```c
int xlink_send(xlink_channel_t* ch, const void* data, size_t len);
```

发送一条消息。

- 对于流式传输（TCP、PIPE、Serial）：自动添加 4 字节大端长度前缀
- 对于消息传输（SHM、UDP、File）：直接传递，不加帧头

返回 0 成功，-1 失败。

---

## xlink_recv

```c
int xlink_recv(xlink_channel_t* ch, void* buf, size_t* len);
```

接收一条消息。

- `*len` 输入时是 buf 容量，输出时是实际数据长度
- 流式传输自动剥离帧头

返回 0 成功，-1 失败/超时。

---

## xlink_write / xlink_read

```c
int xlink_write(xlink_channel_t* ch, const void* data, size_t len);
int xlink_read(xlink_channel_t* ch, void* buf, size_t len, int timeout_ms);
```

底层流式 I/O，跳过 framing 层。用于需要自定义消息边界的场景。

---

## xlink_wait

```c
int xlink_wait(xlink_channel_t** chans, int n, int timeout_ms);
```

多路等待——监听多个通道，返回第一个有数据可读的通道索引。

| 参数 | 说明 |
|------|------|
| `chans` | 通道指针数组 |
| `n` | 通道数量 |
| `timeout_ms` | 超时（毫秒），`-1`=无限等待，`0`=轮询一次立即返回 |

**返回值：**
- `≥ 0` — 有数据可读的通道索引
- `-1` — 超时（无数据）
- `-2` — 参数错误（NULL 指针、`n ≤ 0`、NULL 元素），此时 `errno` 设为 `EINVAL`

**等待策略：**
- **纯 FD 通道**（所有通道有文件描述符）：单次 `poll()` 调用
- **混合 / 纯 SHM 通道**（含 `fd < 0` 且支持 `peek` 的通道）：周期性 `poll()` + `peek()` 循环

**使用示例：**
```c
xlink_channel_t* chans[2] = { pipe_ch, shm_rx };
int ready = xlink_wait(chans, 2, 5000);  // 5秒超时
if (ready >= 0) {
    // chans[ready] 有数据
    xlink_recv(chans[ready], buf, &len);
}
```

---

## xlink_peek

```c
int xlink_peek(xlink_channel_t* ch, size_t* avail);
```

查看可读数据量而不消费。

---

## xlink_close

```c
void xlink_close(xlink_channel_t* ch);
```

关闭通道，释放资源。可接受 NULL。

---

## xlink_errstr / xlink_type_str / xlink_dump

```c
const char* xlink_errstr(xlink_channel_t* ch);
const char* xlink_type_str(xlink_type_t t);
void        xlink_dump(xlink_channel_t* ch, int fd);
```

错误描述、类型名、调试转储。

---

## 选项结构体

```c
typedef struct {
    uint32_t flags;       /* XLINK_CREATE | XLINK_SERVER | XLINK_NONBLOCK | XLINK_BROADCAST */
    size_t   buf_size;    /* 缓冲区大小 */
    int      timeout_ms;  /* 接收超时，-1=阻塞 */
    union {
        struct { int mode;              } shm;     /* 0=compete, 1=broadcast */
        struct { int backlog;           } tcp;     /* listen() backlog */
        struct { int baud; int bits; int parity; int stop; } serial;
        struct { const char* iface; int ttl; } mcast;
    };
} xlink_opt_t;

#define XLINK_OPT_DEFAULT \
    (xlink_opt_t){ .flags = 0, .buf_size = 0, .timeout_ms = -1, .shm = {0} }

> **注意：** 此宏扩展为 C99 复合字面量（compound literal），在**声明**和**赋值**语境中均可用。请不要用于 static 变量初始化（C89 限制）。
```

---

### 标志位说明

| 标志 | 作用 |
|------|------|
| `XLINK_CREATE` | 创建资源（FIFO、SHM 段、文件），而不是打开已有资源 |
| `XLINK_SERVER` | TCP：创建服务端监听 |
| `XLINK_NONBLOCK` | 非阻塞模式。`xlink_recv()` 在无数据时立即返回 -1 |
| `XLINK_BROADCAST` | SHM：创建广播段（多个消费者） |

**NONBLOCK 行为因传输类型而异：**

| 传输 | NONBLOCK 效果 |
|------|--------------|
| PIPE | `read()` 在无数据时返回 EAGAIN |
| TCP | `read()` 在无数据时返回 EAGAIN |
| SERIAL | 启用 O_NONBLOCK，无数据时 `read()` 返回 EAGAIN |
| SHM | 内部使用 `shm_read()`（非阻塞版本）而非 `shm_readn()` |
| UDP | `recvfrom()` 在无数据时返回 EAGAIN |
| FILE | 设置 O_NONBLOCK，但对普通文件无实际影响（POSIX 规定） |

### 已知问题

参见 [`docs/known-issues.md`](known-issues.md) 了解当前已知问题和限制。

---

## 传输类型枚举

```c
typedef enum {
    XLINK_SHM,      /* POSIX shared memory */
    XLINK_PIPE,     /* Named pipe (FIFO)   */
    XLINK_TCP,      /* TCP stream          */
    XLINK_UDP,      /* UDP datagram        */
    XLINK_SERIAL,   /* Serial port         */
    XLINK_RTSP,     /* RTSP stream         */
    XLINK_FILE,     /* File I/O            */
} xlink_type_t;
```

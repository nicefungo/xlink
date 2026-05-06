# P0 短期改进

## 1. TCP 错误路径 xlink_errstr 补充

### 现状

`tcp_backend.c` 中，`write_framed()` 和 `read_framed()` 在出错时设置 `errno` 但不调用 `snprintf(ch->errbuf)`。调用方通过 `xlink_errstr()` 获取到的错误信息依赖 `strerror(errno)`，对于 TCP 场景不够具体。

### 受影响代码路径

```c
// tcp_backend.c — write_framed()
write(peer->fd, iov, iovcnt) 失败
  → 设置 errno
  → 返回 -1
  → 上层：xlink_send() 返回 -1
  → 用户：xlink_errstr(ch) → strerror(errno) → "Connection reset by peer"

// tcp_backend.c — read_framed()
read(peer->fd, ...) 失败
  → 设置 errno
  → 返回 -1
  → 上层：xlink_recv() 返回 -1
  → 用户：xlink_errstr(ch) → strerror(errno)
```

### 修改方案

在 `write_framed()` 和 `read_framed()` 的 `fail` 标签前添加：

```c
// write_framed
write(peer->fd, iov, iovcnt);
if (ret < 0) {
    snprintf(ch->errbuf, XLINK_ERRBUF_SIZE,
             "tcp: write: %s (fd=%d)", strerror(errno), peer->fd);
    goto fail;
}

// read_framed  
read(peer->fd, buf, need);
if (ret <= 0) {
    if (ret == 0)
        snprintf(ch->errbuf, XLINK_ERRBUF_SIZE,
                 "tcp: disconnected (fd=%d)", peer->fd);
    else
        snprintf(ch->errbuf, XLINK_ERRBUF_SIZE,
                 "tcp: read: %s (fd=%d)", strerror(errno), peer->fd);
    goto fail;
}
```

### 验证

- `tcp disconnect` 测试应检查 `xlink_errstr()` 包含 "disconnected"
- `tcp write fail` 测试应检查 `xlink_errstr()` 包含具体错误原因

---

## 2. read_exact 内部超时保护

### 现状

`read_exact()` 在 `tcp_backend.c` 中被调用时，如果传入了 `-1`（无限超时）且某个 `read()` 返回 EAGAIN，代码会 `poll()` 等待无限长时间。对于半开 TCP 连接（对端崩溃但未发送 FIN），这会导致永久挂起。

```c
// xlink.c — framer read
read_exact(&ch->priv->reader, ...)  // 内部调用 tcp_backend_read()
  → tcp_backend_read(fd, buf, n)   // 直接 read()
  → EAGAIN
  → poll(fd, POLLIN, -1)           // ── 永久挂起（半开连接）──
```

### 修改方案

给 `tcp_backend_read()` 添加内部截止时间：

```c
ssize_t tcp_backend_read(void *ctx, int fd, void *buf, size_t n) {
    // 对于阻塞模式且已有部分数据的场景，设置 30s 内部超时
    if (timeout_ms < 0) {  // 无限超时
        // 带内部超时的 poll 循环
        int64_t deadline = now_ms() + 30000;  // 30s 保护
        do {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int remaining = (int)(deadline - now_ms());
            if (remaining <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            ret = poll(&pfd, 1, min(remaining, 1000));
        } while (ret == 0);
    }
}
```

或者更简单的方案：为 `read_exact` 上下文添加超时字段：

```c
// 在 xlink_channel_t 或 backend priv 中添加
struct {
    int64_t deadline;   // 当前操作的截止时间
} read_timeout;
```

### 开放问题

- 30s 是否合理？是否应该可配置？
- 对于 SHM 后端不适用（没有 poll/read 阻塞问题）
- File 后端也不适用（read 不会阻塞）

---

## 3. NONBLOCK TCP send EAGAIN 重试

### 现状

`write_framed()` 在 `O_NONBLOCK` 模式下，`writev()` 返回 EAGAIN 时直接走 `fail` 路径断开连接。但在非阻塞模式下，EAGAIN 是正常现象——发送缓冲区满了，应该重试而不是断开。

```c
// tcp_backend.c — write_framed()
do {
    ret = writev(peer->fd, iov, iovcnt);
} while (ret < 0 && errno == EINTR);  // ── 只重试 EINTR，不重试 EAGAIN

if (ret < 0) {
    // 直接走到 fail：
    // errno == EAGAIN 时也会断开！
    goto fail;
}
```

### 修改方案

在 `while` 条件中添加 `EAGAIN` 的重试逻辑：

```c
int retries = 0;
#define MAX_EAGAIN_RETRIES 100
do {
    ret = writev(peer->fd, iov, iovcnt);
    if (ret < 0 && errno == EAGAIN) {
        if (++retries > MAX_EAGAIN_RETRIES)
            break;  // 达到最大重试次数，断开
        struct pollfd pfd = { .fd = peer->fd, .events = POLLOUT };
        poll(&pfd, 1, 10);  // 等待发送缓冲区可写
        continue;
    }
} while (ret < 0 && errno == EINTR);
```

### 注意事项

- 仅对 `O_NONBLOCK` 模式有意义（阻塞模式下 writev 不会返回 EAGAIN）
- 需要区分真正的 send 失败（EPIPE, ECONNRESET）和缓冲区满（EAGAIN/EWOULDBLOCK）
- 最大重试次数和 poll 等待时间需要调优

### 验证

- `O_NONBLOCK` TCP channel，填满发送缓冲区后继续 send → 不应断开，应重试
- `O_NONBLOCK` TCP channel，对端断开后 send → 应返回 -1（不无限重试）

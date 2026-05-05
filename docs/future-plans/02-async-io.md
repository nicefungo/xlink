# 异步 I/O 支持（io_uring / epoll）

## 动机

当前 xlink 的 I/O 模型是同步阻塞 + `xlink_wait()` 轮询：

- `xlink_wait()` 在混合后端场景下使用 `poll()` + `usleep(5000)` 回退
- 纯 SHM 场景下完全依赖 `usleep(5000)` 轮询
- 没有事件驱动机制，CPU 利用率低（5ms 轮询间隔意味着每秒 200 次系统调用）

对于高性能或低延迟场景，需要一个真正的事件驱动异步 I/O 层。

## 设计方案

### io_uring（Linux 5.1+）

```c
// 异步操作描述符
typedef struct xlink_async_op {
    enum {
        XLINK_AIO_READ,
        XLINK_AIO_WRITE,
        XLINK_AIO_ACCEPT,
        XLINK_AIO_CONNECT
    } type;
    xlink_channel_t *ch;
    void *buf;
    size_t len;
    void (*on_complete)(struct xlink_async_op *op, int result);
    void *user_data;
} xlink_async_op_t;

// 异步 I/O 引擎
typedef struct xlink_aio_engine {
    int ring_fd;                    // io_uring fd
    struct io_uring ring;           // io_uring instance
    int (*submit)(xlink_async_op_t *op);
    int (*poll)(int timeout_ms);    // 返回完成的操作数
    xlink_async_op_t *(*next_done)(void);
} xlink_aio_engine_t;
```

### 兼容层

在不支持 io_uring 的平台上回退到 epoll：

```c
xlink_aio_engine_t *xlink_aio_create(void);
// 内部判断：
//   Linux 5.1+ → io_uring
//   older Linux → epoll
//   其他       → NULL（fallback 到同步模式）
```

### 集成到现有 API

```c
// 新 API：异步 wait
int xlink_wait_async(xlink_channel_t **chans, int n,
                     int timeout_ms, xlink_aio_engine_t *engine);

// 或：事件循环模式（替代 xlink_wait 轮询）
int xlink_run(xlink_channel_t **chans, int n,
              xlink_aio_engine_t *engine,
              void (*on_event)(xlink_channel_t *ch, int event, void *arg),
              void *arg);
```

## 实现路径

### Phase 1: 最小可行

- [ ] 封装 `io_uring` 基础操作（submit/poll/complete）
- [ ] 实现 `xlink_aio_engine_t` 创建和销毁
- [ ] 支持 `read` / `write` 操作
- [ ] 在 TCP 后端中集成异步读写
- [ ] 基准测试对比同步 vs 异步（延迟和吞吐量）
- [ ] 测试：异步 echo server

### Phase 2: 完善

- [ ] `accept` / `connect` 异步操作
- [ ] epoll fallback 实现
- [ ] 超时取消（`io_uring_prep_cancel`）
- [ ] 多 ring 支持（多线程提交）
- [ ] 错误处理和日志

### Phase 3: 优化

- [ ] 固定缓冲区（registered buffers）减少内存拷贝
- [ ] 文件注册（registered files）减少文件描述符开销
- [ ] `IOSQE_IO_LINK` 链式操作
- [ ] `IORING_SETUP_SQPOLL` 减少系统调用

## 依赖

- Linux 5.1+（io_uring）—— 主要平台
- `liburing`（可选，推荐使用）
- epoll fallback 无额外依赖

## 开放问题

1. **API 简洁性**：异步 API 需要回调或 future，是否会破坏 xlink 的简洁 API 哲学？
2. **线程模型**：xlink 设计目标是单线程。io_uring 是否应该使用 SQPOLL（内核侧轮询）来保持单线程？
3. **与现有 xlink_wait() 共存**：是否需要提供透明升级路径？用户代码是否无需修改即可获得异步性能？
4. **SHM 后端适配**：SHM 不需要真正的异步 I/O，但需要与事件循环集成。通知机制（eventfd + io_uring）？

## 关联文档

- [插件化架构](01-plugins-arch.md) — 异步引擎可作为插件系统的核心基础设施
- [性能优化](04-performance.md) — 异步 I/O 是 zero-copy 和批量化优化的前提
- [TLS 加密通信层](03-tls-security.md) — TLS 握手需要异步操作

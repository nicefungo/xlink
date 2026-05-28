# 异步 I/O 支持（io_uring / epoll）— xlink v2.0 核心

> 优先级：P1 → **P0**（与插件化架构并列，共同构成 v2.0）
> 依赖：插件化架构（Phase 1 完成即可开始） | 被依赖：TLS、高性能连接
> 预计工作量：约 2 周

---

## 1. 动机

### 1.1 当前状态

xlink 的 I/O 模型如下：

| 后端 | .read 实现 | 超时支持 | CPU 开销 |
|------|-----------|---------|---------|
| SHM | `shm_peek()` 轮询，500µs 间隔 | ✅ | 中等（轮询） |
| Pipe | `poll()` + `read()` | ✅ | 低 |
| TCP | `poll()` + `recv()` | ✅ | 低 |
| UDP | `poll()` + `recvfrom()` | ✅ | 低 |
| Serial | `poll()` + `read()` | ✅ | 低 |
| File | `poll()` → 立即返回（Linux 文件始终就绪） | ⚠️ 忽略 | 低 |

`xlink_wait()` 在纯 SHM 场景下使用 `usleep(5000)` 轮询，每秒约 200 次唤醒。在混合场景下，轮询间隔 5-100ms。虽然对嵌入式场景可接受，但在高吞吐场景（>10K msg/s）下 CPU 开销不可忽略。

### 1.2 目标场景

| 场景 | 当前问题 | 异步 I/O 解决 |
|------|---------|-------------|
| TCP 服务器 10K 连接 | `poll()` 的 O(n) 扫描 | `epoll` 的 O(1) 事件通知 |
| 视频帧管道（SHM→TCP） | 轮询延迟 5ms | eventfd + io_uring 零延迟 |
| 高吞吐日志（Pipe→File） | 上下文切换开销 | io_uring 批量提交，减少系统调用 |
| 串口数据采集 | poll 超时 cpu 浪费 | epoll 边沿触发 |
| TLS 握手 | 同步 connect/accept 阻塞 | 异步连接 + 异步握手 |

### 1.3 为什么不是 libuv / libevent？

xlink 的目标是**零外部依赖、嵌入式友好**。libuv 是大型库（~40K 行），引入它会破坏 xlink 的极简哲学。我们只需要轻量封装，直接使用内核接口。

---

## 2. 设计方案

### 2.1 异步引擎抽象

不绑定任何特定实现，提供统一的事件驱动接口：

```c
/* ─── 异步引擎类型 ─── */
typedef enum {
    XLINK_AIO_AUTO = 0,    /* 自动选择最优     */
    XLINK_AIO_IOURING,     /* Linux io_uring   */
    XLINK_AIO_EPOLL,       /* Linux epoll       */
    XLINK_AIO_POLL,        /* POSIX poll（回退） */
} xlink_aio_type_t;

/* ─── 异步操作类型 ─── */
typedef enum {
    XLINK_OP_READ,         /* xlink_read 异步版  */
    XLINK_OP_WRITE,        /* xlink_write 异步版 */
    XLINK_OP_ACCEPT,       /* TCP accept         */
    XLINK_OP_CONNECT,      /* TCP connect        */
    XLINK_OP_CLOSE,        /* 异步关闭           */
} xlink_aio_op_kind_t;

/* ─── 完成回调 ─── */
typedef void (*xlink_aio_callback_t)(
    xlink_channel_t *ch,
    xlink_aio_op_kind_t kind,
    int result,             /* bytes or error */
    void *user_data
);

/* ─── 异步操作请求 ─── */
typedef struct xlink_aio_req {
    xlink_aio_op_kind_t   kind;
    xlink_channel_t      *ch;
    void                 *buf;
    size_t                len;
    xlink_aio_callback_t  on_complete;
    void                 *user_data;
} xlink_aio_req_t;

/* ─── 异步引擎实例 ─── */
typedef struct xlink_aio xlink_aio_t;
```

### 2.2 引擎 API

```c
/* 创建异步引擎 */
xlink_aio_t *xlink_aio_create(xlink_aio_type_t type);

/* 提交一个异步操作（立即返回，完成时回调） */
int xlink_aio_submit(xlink_aio_t *aio, xlink_aio_req_t *req);

/* 运行事件循环（阻塞直到有完成事件） */
int xlink_aio_run(xlink_aio_t *aio, int timeout_ms);

/* 运行单次迭代（非阻塞，立即返回） */
int xlink_aio_run_once(xlink_aio_t *aio);

/* 停止事件循环 */
void xlink_aio_stop(xlink_aio_t *aio);

/* 销毁引擎 */
void xlink_aio_destroy(xlink_aio_t *aio);

/* 查询引擎类型 */
xlink_aio_type_t xlink_aio_type(xlink_aio_t *aio);
```

### 2.3 io_uring 实现（Linux 5.1+）

```c
/* 内部实现（aio_uring.c） */
struct xlink_aio {
    xlink_aio_type_t    type;
    struct io_uring     ring;
    int                 running;

    /* SQE → 请求的映射表 */
    xlink_aio_req_t    *pending[256];  /* 最多 256 个并发请求 */

    /* CQE 处理 */
    void (*process_cqe)(xlink_aio_t *aio, struct io_uring_cqe *cqe);
};
```

关键操作映射：

| xlink 操作 | io_uring opcode | 说明 |
|-----------|----------------|------|
| `XLINK_OP_READ` | `IORING_OP_READ` | 异步读取 fd |
| `XLINK_OP_WRITE` | `IORING_OP_WRITE` | 异步写入 fd |
| `XLINK_OP_ACCEPT` | `IORING_OP_ACCEPT` | TCP accept |
| `XLINK_OP_CONNECT` | `IORING_OP_CONNECT` | TCP connect |
| `XLINK_OP_CLOSE` | `IORING_OP_CLOSE` | 关闭 fd |

### 2.4 epoll 实现（Linux 2.6+，旧内核回退）

```c
/* 内部实现（aio_epoll.c） */
struct xlink_aio {
    xlink_aio_type_t    type;
    int                 epfd;
    int                 running;

    /* eventfd 用于唤醒事件循环 */
    int                 event_fd;

    /* pending 队列 */
    xlink_aio_req_t    *pending_head;
    xlink_aio_req_t    *pending_tail;
};
```

epoll 版是"伪异步"：`submit()` 将请求入队，`run()` 中执行实际的 `read()/write()` 并调用回调。虽然底层 I/O 仍是同步的，但 API 层面提供了统一的异步接口。事件通知（数据到达）是真正异步的（epoll）。

### 2.5 集成到 xlink_wait()

**新增 API**：事件驱动的 `xlink_wait()`：

```c
/* 传统轮询版（保持兼容） */
int xlink_wait(xlink_channel_t **chans, int n, int timeout_ms);

/* 新增：异步事件版 */
int xlink_wait_aio(xlink_channel_t **chans, int n,
                   int timeout_ms, xlink_aio_t *aio);
/* 内部用 epoll/io_uring 替代 poll() + usleep() */

/* 新增：事件回调版（零轮询） */
typedef void (*xlink_event_cb)(xlink_channel_t *ch, int idx, void *arg);

int xlink_run(xlink_channel_t **chans, int n,
              xlink_aio_t *aio,
              xlink_event_cb on_data, void *arg);
/* 事件驱动的 xlink_wait() 替代：有新数据时回调，无数据时阻塞 */
```

### 2.6 SHM 异步唤醒

SHM 没有 fd，无法被 epoll/io_uring 直接监听。解决方案：eventfd 唤醒机制。

```c
/* SHM 后端增强：可选的 eventfd */
typedef struct {
    char  name[64];
    int   event_fd;    /* 可选：有新数据时 write(event_fd) */
} shm_priv_t;

/* 写入端：数据写入后通知 */
static int shm_backend_send(xlink_channel_t *ch, ...) {
    // ... 正常写入 ...
    if (p->event_fd >= 0) {
        uint64_t one = 1;
        write(p->event_fd, &one, sizeof(one));  /* 唤醒等待者 */
    }
    return 0;
}

/* 读取端：epoll 监听 event_fd + shm_peek() */
// xlink_wait_aio() 在 event_fd 可读时检查 shm_peek()
```

这使得 SHM 在异步模式下也是事件驱动的，不再需要 `usleep(5000)` 轮询。

---

## 3. 实现路径

### Phase 1: 引擎抽象 + epoll 实现

**目标**：可工作的异步引擎，涵盖所有 fd-based 后端

- [ ] 实现 `aio.h`：统一引擎接口（`xlink_aio_t` 定义）
- [ ] 实现 `aio_epoll.c`：epoll 版本引擎
  - `xlink_aio_create(XLINK_AIO_EPOLL)` 创建 epoll 实例
  - `xlink_aio_submit()` 将请求入队 + 注册 epoll 事件
  - `xlink_aio_run()` 事件循环（`epoll_wait` + 执行回调）
- [ ] 实现 `xlink_wait_aio()`：用 epoll 替代 poll()
- [ ] 基准测试：异步 xlink_wait 对比同步 xlink_wait

**验证**：新测试 `test_aio_epoll`。异步 TCP echo server。

### Phase 2: io_uring 实现

**目标**：真正的内核级异步 I/O

- [ ] 实现 `aio_uring.c`：io_uring 版本引擎
  - `xlink_aio_create(XLINK_AIO_IOURING)` 初始化 io_uring
  - `xlink_aio_submit()` 提交 SQE
  - `xlink_aio_run()` → `io_uring_wait_cqe` + 回调
- [ ] 实现固定缓冲区（`IORING_REGISTER_BUFFERS`）减少拷贝
- [ ] 基准测试对比：epoll vs io_uring（延迟 / 吞吐）

**验证**：`test_aio_uring`。压力测试 10K 并发连接。

### Phase 3: 完善

- [ ] SHM eventfd 唤醒机制
- [ ] `xlink_run()` 事件回调模式（零轮询主循环）
- [ ] 异步 connect/accept（TCP 服务器零阻塞启动）
- [ ] 超时取消（`io_uring_prep_cancel` / epoll timerfd_*)
- [ ] 引擎自动选择（`XLINK_AIO_AUTO` → 检测内核版本）
- [ ] macOS kqueue 支持（可选，P2）
- [ ] 文档：异步编程指南 + 迁移指南

---

## 4. 性能预期

| 指标 | 当前（轮询） | epoll | io_uring |
|------|-----------|-------|----------|
| SHM 延迟 | 0-5ms（轮询） | <100µs（eventfd） | <100µs（eventfd） |
| TCP 延迟 | 0-1ms（poll） | <100µs（epoll） | <50µs（io_uring） |
| CPU 空闲时 | 200 wakeups/s | 0（事件驱动） | 0（事件驱动） |
| 10K TCP 连接 | O(n) poll | O(1) epoll | O(1) io_uring |
| 系统调用/msg | 2-3（poll+read/write） | 2-3（epoll+read/write） | 1（io_uring batch） |

### 关键指标：从轮询到事件驱动

```
当前（混合后端 xlink_wait）:
  ┌──────┐     ┌──────┐     ┌──────┐     ┌──────┐
  │ poll │────▶│sleep │────▶│ peek │────▶│ poll │──▶ ... (每 5-100ms 循环)
  └──────┘     └──────┘     └──────┘     └──────┘
  系统调用        CPU空转      系统调用       系统调用

异步（epoll + eventfd）:
  ┌──────┐                           ┌──────┐
  │ wait │──────────────────────────▶│ data │  (仅在数据到达时唤醒)
  └──────┘     (阻塞，零 CPU)         └──────┘
```

---

## 5. 依赖

| 依赖项 | 必须？ | 说明 |
|--------|-------|------|
| Linux 5.1+ | io_uring 需要 | 旧内核回退 epoll |
| `liburing` | 推荐 | 简化 io_uring 使用（可选，可直接使用 syscall） |
| 插件化架构 | 必须 | 异步引擎本身作为插件加载 |
| `pthread` | 可选 | 多线程事件循环（Phase 3） |

---

## 6. 与插件化架构的协同设计

异步 I/O 引擎**不硬编码进 xlink**——它作为插件加载：

```
xlink 启动时：
  1. 检测内核版本
  2. 若 Linux 5.1+ → 尝试加载 io_uring 插件
  3. 若失败 → 加载 epoll 插件
  4. 若都失败 → 回退到传统 poll 模式（现有行为）
```

这意味着：
- 用户不需要任何代码更改即可获得异步性能
- 不同的部署环境自动选择最佳引擎
- 未来添加 kqueue（macOS）不需要改动核心代码

---

## 7. 开放问题

1. **默认行为**：xlink 是否应该默认启用异步引擎？还是需要显式调用 `xlink_aio_create()`？
   - 推荐：默认启用（`XLINK_AIO_AUTO`），用户可显式关闭（`xlink_aio_create(NULL)`）

2. **线程模型**：事件循环在哪个线程运行？
   - 推荐：保持单线程模型（主线程跑事件循环），符合 xlink 嵌入式定位
   - 可选：`xlink_aio_run()` 在新线程（使用 `pthread`）跑

3. **回调 vs 协程**：异步回调地狱怎么避免？
   - 推荐：保持回调模式（C 语言最佳实践）
   - 未来可选：`setjmp/longjmp` 协程（ucontext），但增加复杂度

4. **SHM eventfd 是强制还是可选？**
   - 推荐：可选（`xlink_opt_t` 中加标志位 `XLINK_AIO_NOTIFY`），保持向后兼容

5. **macOS / BSD 支持**：kqueue 是 Phase 3 还是永不实现？
   - 当前：epoll + io_uring 覆盖 99% 的 Linux 嵌入式场景，kqueue 是 P2

---

## 8. 关联文档

- [插件化架构](01-plugins-arch.md) — 异步 I/O 的前提，两个计划共同构成 v2.0 核心
- [TLS 加密通信层](03-tls-security.md) — TLS 握手依赖异步连接操作
- [性能优化](04-performance.md) — zero-copy 和批量化依赖 io_uring 的 registered buffers
- [技术报告](/home/admin/xlink/docs/technical-report.md) — 当前 v1.0 的完整说明

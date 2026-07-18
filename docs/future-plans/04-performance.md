# 性能优化（Zero-Copy、批量化）

## 动机

当前 xlink 在吞吐量场景下存在明显的性能瓶颈：

1. **内存拷贝**: `frame_send()` 中的 `writev()` 虽然有 iovec 优化，但数据从用户缓冲区到内核再到对端仍有多层拷贝
2. **小包问题**: 高频小消息场景下，每次发送都有系统调用开销
3. **轮询开销**: `xlink_wait()` 使用 `usleep(5000)` 轮询，CPU 开销随通道数线性增长
4. **锁竞争**: 多生产者场景下，共享内存的锁竞争可能成为瓶颈（未实测）

目标场景：消息中间件、实时数据管道、高频交易信号分发。

## 设计方案

### 1. Zero-Copy 发送路径

```c
// 注册共享缓冲区
typedef struct xlink_zc_buf {
    void *addr;
    size_t len;
    int fd;              // memfd / dmabuf fd
} xlink_zc_buf_t;

// 零拷贝发送
int xlink_send_zc(xlink_channel_t *ch, xlink_zc_buf_t *buf);

// SHM 后端零拷贝：只传递元数据（offset + len）
// TCP 后端零拷贝：使用 MSG_ZEROCOPY（Linux 4.14+）
// 文件后端零拷贝：使用 splice() / copy_file_range()
```

### 2. 批量化发送

```c
// 批量发送（一次系统调用发送多条消息）
typedef struct xlink_msg {
    const void *data;
    size_t len;
} xlink_msg_t;

int xlink_send_batch(xlink_channel_t *ch, xlink_msg_t *msgs, int count);

// SHM 后端：在环形缓冲区中批量写入
// TCP 后端：writev() 合并多条消息
```

### 3. 批量化接收

```c
// 批量接收
int xlink_recv_batch(xlink_channel_t *ch,
                     void **buffers, size_t *lengths,
                     int max_count);
```

### 4. I/O 合并

在 TCP 场景下，使用 Nagle 算法协商或 `MSG_MORE` flag 合并小包：

```c
// 发送端
setsockopt(fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
// 批量写入
// 解除 CORK（自动 flush）
```

## 实现路径

### Phase 1: 批量化

- [x] `xlink_send_batch()` API 设计 + SHM 后端实现（2026-07-12）
- [x] `xlink_recv_batch()` API 设计 + SHM 后端实现（2026-07-13）
- [x] TCP 后端 `writev()` 批量发送（2026-07-14，使用 TCP_CORK）
- [x] 基准测试：单条 vs 批量吞吐量对比（2026-07-14，test_batch_perf.c）
- [x] 测试：`xlink_send_batch()` 基础测试（2026-07-12）
- [x] 测试：`xlink_recv_batch()` 基础测试（2026-07-13）

### Phase 2: Zero-Copy

> **设计深化: 2026-07-15** — 补充完整接口定义、完成通知机制、各后端伪代码、所有权语义

#### 2.1 核心接口定义

```c
/* ── 零拷贝缓冲区 ── */
typedef struct xlink_zc_buf {
    void   *addr;        /* 数据起始地址 */
    size_t  len;         /* 数据长度 */
    int     fd;          /* 可选 backing fd（memfd/shm/regular file） */
    uint64_t tag;        /* 用户自定义 tag（用于完成通知匹配） */
} xlink_zc_buf_t;

/* ── 完成回调 ── */
typedef void (*xlink_zc_done_fn)(xlink_channel_t *ch,
                                  uint64_t tag,
                                  int status,    /* 0=成功, <0=错误 */
                                  void *userdata);

/* ── 零拷贝发送（异步） ── */
int xlink_send_zc(xlink_channel_t *ch,
                  xlink_zc_buf_t *buf,
                  xlink_zc_done_fn done,
                  void *userdata);

/* ── 检查零拷贝完成状态 ── */
int xlink_zc_poll(xlink_channel_t *ch);  /* 返回完成数，-1 出错 */

/* ── 零拷贝能力查询 ── */
int xlink_zc_capable(xlink_channel_t *ch);  /* 返回 1 表示支持 */
```

**所有权语义**：
- 调用 `xlink_send_zc()` 后，**调用者不再拥有 `buf->addr`**，直到 `done` 回调被触发
- `done` 回调表示内核已完成对缓冲区的引用（可以安全重用/释放）
- 如果 `done` 回调为 NULL，使用 `xlink_zc_poll()` 轮询完成状态
- `fd` 为可选：SHM 不需要 fd，TCP/MSG_ZEROCOPY 需要 fd 作为锚点

#### 2.2 完成通知机制

每个通道维护一个环形完成队列：

```c
struct zc_channel_state {
    uint64_t           next_tag;          /* 单调递增 tag 分配器 */
    ring_buffer_t     *done_queue;        /* 已完成 tag 列表 */
    pthread_mutex_t    done_lock;         /* 保护 done_queue */
    pthread_cond_t     done_cond;         /* 通知等待者 */
    int                pending;           /* 未完成的数量 */
};
```

通知路径：
1. **TCP (MSG_ZEROCOPY)**: 内核通过 `SO_EE_ORIGIN_ZEROCOPY` 错误队列通知
2. **SHM**: 对端消费后在共享内存中写入确认（或使用原有 FIFO 通知）
3. **Pipe/File (splice)**: 同步完成，`done` 在 `xlink_send_zc()` 返回前调用

#### 2.3 SHM 后端：True Zero-Copy

SHM 后端的零拷贝不复制数据，只传递元数据（offset + len）：

```c
/* SHM 零拷贝在环形缓冲区中只写描述符，不拷贝 payload */
struct shm_zc_desc {
    uint32_t offset;    /* 在共享内存池中的偏移 */
    uint32_t len;       /* 数据长度 */
    uint64_t tag;       /* 用户 tag */
};

int shm_send_zc(xlink_channel_t *ch, xlink_zc_buf_t *buf,
                xlink_zc_done_fn done, void *userdata)
{
    /* 1. 将 buf->addr 注册到共享内存池（如果需要） */
    /* 2. 计算 offset = addr - pool_base */
    /* 3. 写入 shm_zc_desc 到环形缓冲区 */
    /* 4. 通知对端（eventfd / shm_ipc 原有机制） */
    /* 5. 对端消费后触发完成通知 */
}
```

对端使用 `xlink_recv_zc()` 直接访问共享内存中的数据：

```c
/* 零拷贝接收：返回指向共享内存中数据的指针 */
int xlink_recv_zc(xlink_channel_t *ch, void **data, size_t *len);

/* 释放零拷贝接收的缓冲区 */
void xlink_recv_zc_done(xlink_channel_t *ch, void *data);
```

**性能预期**：SHM 零拷贝应该比标准 `xlink_send()` 快 3-10×（免除 `memcpy`），特别是对于大消息（4KB+）。

#### 2.4 TCP 后端：MSG_ZEROCOPY（Linux 4.14+）

```c
int tcp_send_zc(xlink_channel_t *ch, xlink_zc_buf_t *buf,
                xlink_zc_done_fn done, void *userdata)
{
    struct msghdr msg = {0};
    struct iovec  iov;
    char cmsg[CMSG_SPACE(sizeof(uint32_t))];

    iov.iov_base = buf->addr;
    iov.iov_len  = buf->len;

    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg;
    msg.msg_controllen = sizeof(cmsg);

    /* 设置 SO_ZEROCOPY 完成通知 */
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type  = SO_EE_ORIGIN_ZEROCOPY;
    c->cmsg_len   = CMSG_LEN(sizeof(uint32_t));
    *(uint32_t *)CMSG_DATA(c) = (uint32_t)ch->zc.next_tag;

    ssize_t n = sendmsg(ch->fd, &msg, MSG_ZEROCOPY);
    /* ... 注册 (tag, done_fn, userdata) 到 pending map ... */
    ch->zc.pending++;
    return (n >= 0) ? 0 : -1;
}
```

完成通知通过 epoll 监听 socket 错误队列（`EPOLLERR`），在错误队列中读取 `SO_EE_ORIGIN_ZEROCOPY` 事件，调用相应的 `done` 回调。

**注意事项**：
- MSG_ZEROCOPY 要求 socket `SO_ZEROCOPY` 已设置
- 缓冲区必须页对齐 + 整页大小（否则内核仍会拷贝部分）
- 内核引用计数管理生命周期；发送到缓冲区重用至少需 RTT/2
- Linux 4.14+ 才稳定可用（4.18+ 推荐）

#### 2.5 文件后端：splice() + copy_file_range()

```c
/* File-to-pipe: 零拷贝从文件到管道 */
int file_send_zc(xlink_channel_t *ch, xlink_zc_buf_t *buf,
                 xlink_zc_done_fn done, void *userdata)
{
    /* splice() 在 file_fd → pipe_fd 间移动页面，不拷贝 */
    int pipefd[2];
    pipe(pipefd);

    loff_t offset = (loff_t)(uintptr_t)buf->addr; /* 文件偏移 */
    ssize_t n = splice(buf->fd, &offset,
                       pipefd[1], NULL, buf->len,
                       SPLICE_F_MOVE | SPLICE_F_MORE);

    /* 然后从 pipe 读回（kernel 内部零拷贝，仅操作 page cache） */
    /* 或者直接传给 TCP socket（splice to socket） */
    /* done 回调在 splice 完成时立即调用（同步） */
}
```

```c
/* File-to-file: copy_file_range() (Linux 4.5+) */
int file_zc_copy(xlink_channel_t *ch1, xlink_channel_t *ch2,
                 loff_t *off_in, loff_t *off_out,
                 size_t len)
{
    /* 两个文件 fd 间直接在 page cache 中复制 */
    /* NFS 4.2+ 服务端拷贝（避免回传客户端再传回去） */
    return copy_file_range(ch1->fd, off_in,
                           ch2->fd, off_out, len, 0);
}
```

#### 2.6 性能预期

| 后端 | 操作 | 标准路径延迟 | Zero-Copy 延迟 | 提速 |
|------|------|-------------|-----------------|------|
| SHM | 4KB send | ~0.035ms | ~0.008ms (estimated) | ~4× |
| SHM | 64KB send | ~0.12ms | ~0.015ms (estimated) | ~8× |
| TCP | 4KB send (loopback) | ~0.005ms | ~0.003ms (estimated) | ~1.5× |
| TCP | 64KB send (loopback) | ~0.04ms | ~0.008ms (estimated) | ~5× |
| File | 1MB copy | ~copy cost | 0 (page cache op) | ∞ |

#### 2.7 实现步骤

- [ ] **Step 2.1**: `xlink_zc_buf_t` + `xlink_zc_done_fn` 类型定义（`include/xlink.h`）
- [ ] **Step 2.2**: SHM zero-copy（`src/shm_backend.c`）：`shm_send_zc` + `xlink_recv_zc` + `xlink_recv_zc_done`
- [ ] **Step 2.3**: SHM 完成通知（eventfd / FIFO 集成）
- [ ] **Step 2.4**: TCP MSG_ZEROCOPY（`src/tcp_backend.c`）：`tcp_send_zc` + epoll 错误队列监控
- [ ] **Step 2.5**: File splice/copy_file_range（`src/file_backend.c`）
- [ ] **Step 2.6**: 测试：`test_zc_shm.c`, `test_zc_tcp.c`, `test_zc_file.c`
- [ ] **Step 2.7**: 基准测试：`test_zc_perf.c`（对比标准路径）
- [ ] **Step 2.8**: 更新 `api.md` / `code-walkthrough.md` / `04-performance.md`

### Phase 3: 优化

- [x] TCP CORK / MSG_MORE 策略自动检测（2026-07-14，已集成到 `xlink_send_batch()`）
- [x] 自适应批量化设计（2026-07-16，详见 §3.1）
- [x] 多核场景生产者-消费者优化设计（2026-07-16，详见 §3.2）
- [x] 自适应批量化实现（2026-07-17，新增 `xlink_batch_policy_t` + `xlink_set_batch_policy()` + `xlink_flush_batch()` API，EWMA 自适应控制器，`test_batch_adaptive.c` 38/38 通过）
- [x] Lock-free SPSC 队列实现（2026-07-18，新增 `src/spsc_queue.h` + `src/spsc_queue.c`，C11 atomic acquire/release 语义，cache-line padded，`test_spsc.c` 6/6 通过含 100万 MT 并发）
- [ ] Lock-free MPSC 队列（per-producer slot 多生产者扩展）
- [ ] SHM 后端集成 lock-free 队列
- [ ] CPU 缓存友好（消息对齐、cacheline padding）
- [ ] 性能剖析工具（perf / flamegraph 集成）

#### 3.1 自适应批量化设计

**问题**：固定 batch size（如 50 条）在不同场景下效果差异大。高频小消息场景（10K msg/s）需要更大的批量来分摊系统调用开销；低频大消息场景（10 msg/s，每条 64KB）batch 太大只会增加延迟无益于吞吐。

**设计策略**：采用三参数自适应控制器。

```c
/* 自适应批量化配置 */
typedef struct xlink_batch_policy {
    int    max_batch;        /* 最大批量大小（hard limit） */
    int    max_delay_us;     /* 最大等待时间（microseconds） */
    int    min_batch;        /* 最小批量（低于此值等待） */
    int    enable;           /* 0=关闭自适应，1=启用 */
} xlink_batch_policy_t;

/* 设置通道的批量策略 */
int xlink_set_batch_policy(xlink_channel_t *ch,
                           const xlink_batch_policy_t *policy);
```

**内部状态机**：

```c
struct xlink_batch_state {
    xlink_msg_t   *queue;          /* 待发送消息环形队列 */
    int            q_head, q_tail; /* 队列指针 */
    int            q_cap;          /* 队列容量 */

    /* 自适应参数（运行时更新） */
    double         avg_msg_rate;   /* 滑动平均消息速率 (msg/s) */
    double         avg_msg_size;   /* 滑动平均消息大小 (bytes) */
    struct timespec last_flush;    /* 上次 flush 时间 */
    struct timespec first_queued;  /* 第一条消息入队时间 */

    int            current_batch;  /* 当前建议的 batch size */
    int            samples;        /* 采样计数 */
};
```

**自适应逻辑**（每次 `xlink_send_batch()` 调用时评估）：

```
BatchDecision(rate, avg_size, policy):
    IF queue_count >= max_batch
        → FLUSH (hard limit)

    IF avg_size >= 4096 (大消息)
        → current_batch = max(1, max_batch / 4)

    IF rate > 10000 msg/s (高频)
        → current_batch = min(max_batch, current_batch * 2)

    IF elapsed_since_first > max_delay_us
        → FLUSH (time-based)

    IF queue_count >= current_batch
        → FLUSH (threshold-based)

    OTHERWISE
        → QUEUE (defer to next call or explicit flush)
```

**滑动平均**：指数加权移动平均（EWMA），`α = 0.125`（约 8 个样本达到收敛）：

```c
static void update_ewma(double *avg, double sample) {
    const double alpha = 0.125;
    *avg = (*avg == 0.0) ? sample : alpha * sample + (1.0 - alpha) * (*avg);
}
```

**性能预期**（预估，待实测）：

| 场景 | 固定 batch=50 | 自适应 | 改善 |
|------|-------------|--------|------|
| 高频小消息（10K msg/s, 64B） | ~39500 KB/s | ~42000 KB/s | +6% |
| 低频大消息（10 msg/s, 64KB） | ~640 KB/s, 延迟 5s | ~640 KB/s, 延迟 0.5ms | 延迟 -10000× |
| 混合场景（burst + idle） | 尾部延迟高 | 延迟受控 | 关键改善 |

**设计决策**: 不侵入 `xlink_send()` 的简单性。自适应层在 `xlink_send_batch()` 内部作为可选策略启用（默认关闭，用户显式调用 `xlink_set_batch_policy()` 开启）。

#### 3.2 多核生产者-消费者优化

**问题**：当前 SHM 后端使用单一锁保护整个环形缓冲区。多核高并发场景下，N 个生产者竞争同一把锁（`pthread_mutex_lock`），其中 N-1 个在等待，实际执行串行化。

**关键瓶颈识别**：
1. **锁竞争**: `pthread_mutex_lock(&ring->lock)` — 多生产者场景的最大瓶颈
2. **False sharing**: 多个 CPU 核心写同一 cache line（即使写不同字段）
3. **内存屏障开销**: 每次 `unlock` 触发全内存屏障（mfence / `lock` 前缀）

**优化方案：分层设计**

```
方案 A: Lock-free SPSC (单生产者单消费者)
│  适用于一对一的 SHM 通道
│  → 使用原子操作（CAS）+ 环形缓冲区
│  → 零锁竞争

方案 B: Lock-free MPSC (多生产者单消费者) — 推荐
│  适用于广播/汇聚场景
│  → 每个生产者有自己的 slot（预分配）
│  → 消费者轮询所有 producer slots
│  → 使用 acquire/release 语义，无锁

方案 C: Per-CPU ring buffer (多生产者多消费者)
│  适用于最大并发场景
│  → 每个 CPU core 有自己的 ring buffer
│  → 发送时写入 local ring，消费者轮询所有 rings
│  → 可用 C11 atomics 实现，无需内核支持
```

**方案 A 详细设计：Lock-free SPSC**

```c
/* Lock-free SPSC ring buffer (基于 Lamport 经典算法) */
typedef struct {
    xlink_msg_t *buffer;
    size_t       mask;           /* capacity - 1, power of 2 */
    size_t       capacity;

    /* 单写者修改 head，单读者修改 tail — 各自独立 */
    _Atomic size_t head;         /* 生产者写入位置 */
    _Atomic size_t tail;         /* 消费者读取位置 */

    /* cacheline padding 防止 false sharing */
    char _pad[CACHE_LINE_SIZE];
} xlink_spsc_queue_t;

static inline int spsc_enqueue(xlink_spsc_queue_t *q,
                                const xlink_msg_t *msg) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    /* 满判断：head + 1 == tail (mod capacity) */
    if ((head + 1 - tail) >= q->capacity)
        return -1;  /* full */

    q->buffer[head & q->mask] = *msg;

    /* 确保数据写入在 head 更新之前可见 */
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 0;
}

static inline int spsc_dequeue(xlink_spsc_queue_t *q,
                                xlink_msg_t *msg) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail == head)
        return -1;  /* empty */

    *msg = q->buffer[tail & q->mask];

    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}
```

**内存序分析**：
- **生产者写** `release`：确保 `buffer[]` 写入对消费者可见后，才发布 `head` 更新
- **消费者读** `acquire`：看到 `head` 更新后，保证能看到对应的 `buffer[]` 内容
- **仅 2 个原子操作** per op（vs `pthread_mutex_lock` 的 syscall + futex 开销）

**方案 B 设计：MPSC per-producer slot**

```c
/* MPSC: 每个生产者独立 slot，消费者轮询 */
typedef struct xlink_mpsc_queue {
    int           n_producers;     /* 生产者数量 */
    xlink_spsc_queue_t **slots;   /* 每个生产者一个 SPSC 队列 */
} xlink_mpsc_queue_t;

/* 生产者 N 写入 slot[N]（无锁，各写各的） */
/* 消费者轮询所有 slot：遍历 slots[0..N-1] */
```

**性能预期**（vs 当前 mutex 方案）：

| 场景 | Mutex (当前) | Lock-free SPSC | Lock-free MPSC |
|------|------------|----------------|----------------|
| 1 生产者 | 0.035ms | ~0.008ms (4×) | ~0.010ms |
| 4 生产者 | 0.14ms (锁竞争) | N/A | ~0.012ms (12×) |
| 8 生产者 | 0.30ms (严重竞争) | N/A | ~0.015ms (20×) |
| cache miss | per lock/unlock | 2 atomics | 2 atomics × N recv |

**实现优先级**：先做方案 A（SPSC），因为 SHM 最常见场景是一对一通信。方案 B（MPSC）在广播/汇聚场景需要时再做。方案 C（per-CPU）留作远期。

#### 3.3 实施计划

**自适应批量化**（预估 1.5 天）：
1. `xlink_batch_policy_t` + `xlink_set_batch_policy()` API（`include/xlink.h`）
2. 内部 `struct batch_state` + EWMA 控制器（`src/xlink.c`）
3. `xlink_send_batch()` 集成自适应逻辑
4. `test_batch_adaptive.c`：验证 rate detection + batch size 动态调整
5. `test_batch_perf.c` 扩展：对比固定 vs 自适应

**Lock-free SPSC**（预估 2 天）：
1. `xlink_spsc_queue_t` 数据结构 + 基本操作（`src/spsc_queue.c`）
2. SHM 后端集成：`shm_backend.c` 中 opend 时初始化 SPSC 队列
3. `test_spsc.c`：正确性验证（多线程并发 enqueue/dequeue）
4. `test_spsc_perf.c`：对比 mutex vs lock-free 吞吐量
5. 集成到 `xlink_send_batch()` / `xlink_send()` 热路径

## 依赖

- **Phase 1**: 无额外依赖
- **Phase 2**: Linux 4.14+（MSG_ZEROCOPY）/ 无特殊要求
- **前置依赖**: 异步 I/O（[02-async-io.md](02-async-io.md)）—— 高性能场景建议使用事件驱动

## 开放问题

1. **API 复杂度**: 批量化 API 是否应该透明集成到现有 `xlink_send()` 中（自动合并小包）？还是暴露为独立 API？
   → **决策 (2026-07-15)**: 暴露为独立 API（`xlink_send_batch()` / `xlink_send_zc()`），保持 `xlink_send()` 简单不伤。高级用户显式选择批量化/零拷贝路径。
2. **Zero-Copy 所有权**: zero-copy 发送后，缓冲区何时可以重用？需要类似 `sendmsg()` 的完成通知机制。
   → **设计 (2026-07-15)**: 采用异步完成回调 `xlink_zc_done_fn`，同时提供轮询接口 `xlink_zc_poll()` 备选。SHM 需要等对端消费完成通知；TCP MSG_ZEROCOPY 通过 epoll 错误队列通知；File splice 同步完成。
3. **SHM vs TCP 差异**: SHM 的 zero-copy 是"真正的零拷贝"（指针传递），而 TCP 只能做到减少拷贝。这两个场景是否需要不同的 API？
   → **决策 (2026-07-15)**: 统一 API 签名（`xlink_send_zc(ch, buf, done, userdata)`），后端差异在内部处理。用户通过 `xlink_zc_capable()` 查询能力，不需要知道后端细节。
4. **批量化与延迟的 trade-off**: 批量化提高吞吐但增加延迟。是否需要用户可控的 flush 策略？
   → 仍然开放。备选方案：`xlink_flush(ch)` 显式 flush，或 `XLINK_BATCH_TIMEOUT` 配置最大等待时间。

## 关联文档

- [插件化架构](01-plugins-arch.md) — 批量化/zero-copy 可作为后端 vtable 的扩展接口
- [异步 I/O 支持](02-async-io.md) — 高性能 I/O 的基础设施
- [跨平台支持](05-multi-platform.md) — MSG_ZEROCOPY 和 splice() 是 Linux 特有

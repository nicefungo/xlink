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

- [ ] `xlink_send_batch()` API 设计 + SHM 后端实现
- [ ] `xlink_recv_batch()` API 设计 + SHM 后端实现
- [ ] TCP 后端 `writev()` 批量发送
- [ ] 基准测试：单条 vs 批量吞吐量对比
- [ ] 测试：大批量消息（10000 msg/s）的可靠性

### Phase 2: Zero-Copy

- [ ] `xlink_zc_buf_t` 接口定义
- [ ] SHM 后端 zero-copy（共享内存传递指针）
- [ ] TCP 后端 `MSG_ZEROCOPY`（Linux 4.14+）
- [ ] 文件后端 `splice()` / `copy_file_range()`
- [ ] 基准测试：zero-copy vs 标准拷贝延迟对比

### Phase 3: 优化

- [ ] TCP CORK / MSG_MORE 策略自动检测
- [ ] 自适应批量化（根据消息频率动态调整 batch size）
- [ ] 多核场景下的生产者-消费者优化
- [ ] CPU 缓存友好（消息对齐、cacheline padding）
- [ ] 性能剖析工具（perf / flamegraph 集成）

## 依赖

- **Phase 1**: 无额外依赖
- **Phase 2**: Linux 4.14+（MSG_ZEROCOPY）/ 无特殊要求
- **前置依赖**: 异步 I/O（[02-async-io.md](02-async-io.md)）—— 高性能场景建议使用事件驱动

## 开放问题

1. **API 复杂度**: 批量化 API 是否应该透明集成到现有 `xlink_send()` 中（自动合并小包）？还是暴露为独立 API？
2. **Zero-Copy 所有权**: zero-copy 发送后，缓冲区何时可以重用？需要类似 `sendmsg()` 的完成通知机制。
3. **SHM vs TCP 差异**: SHM 的 zero-copy 是"真正的零拷贝"（指针传递），而 TCP 只能做到减少拷贝。这两个场景是否需要不同的 API？
4. **批量化与延迟的 trade-off**: 批量化提高吞吐但增加延迟。是否需要用户可控的 flush 策略？

## 关联文档

- [插件化架构](01-plugins-arch.md) — 批量化/zero-copy 可作为后端 vtable 的扩展接口
- [异步 I/O 支持](02-async-io.md) — 高性能 I/O 的基础设施
- [跨平台支持](05-multi-platform.md) — MSG_ZEROCOPY 和 splice() 是 Linux 特有

# xlink v2.1 性能基准测试报告

> 创建: 2026-06-22 | 状态: ✅ 已完成
> 基准代码: `tests/test_aio_perf.c`

## 1. 测试环境

| 属性 | 值 |
|------|-----|
| 内核 | Linux 5.15.0-144-generic (x86_64) |
| 编译器 | cc (GCC) -O2 -g |
| 编译选项 | -Wall -Wextra |
| 引擎类型 | poll (POSIX), epoll (Linux), io_uring (Linux 5.1+) |

## 2. 测试方法

6 个独立基准测试，每个测试在独立进程内运行，使用不同的 Pipe/SHM 通道名避免干扰：

| # | 测试 | 度量 | 迭代次数 |
|---|------|------|----------|
| 1 | Pipe latency (poll) | 1KB round-trip | 100 |
| 2 | Pipe latency (epoll) | 1KB round-trip | 100 |
| 3 | Pipe throughput (3 engines) | 32KB bulk, MB/s | 50 |
| 4 | SHM latency (epoll) | 1KB round-trip | 100 |
| 5 | Multi-channel (epoll) | 4 pipes, sequential | 50 |
| 6 | io_uring latency | 1KB round-trip | 100 |

## 3. 结果

### 3.1 延迟 (1KB Pipe Round-Trip)

| 引擎 | 延迟 (ms) | 相对比率 |
|------|----------|----------|
| poll | 0.004 | 1.00× (基线) |
| io_uring | 0.004 | 1.00× |
| epoll | 0.007 | 1.75× |

**分析**: 在本地 Pipe 场景下，poll 和 io_uring 几乎持平，epoll 略慢。poll 的 syscall 路径最短（单次 `poll()`），io_uring 需要提交 SQE + 等待 CQE（两次 syscall），但在小数据量时 io_uring 的批量提交优势无法体现。

### 3.2 吞吐 (32KB Bulk Pipe)

| 引擎 | MB/s | 相对比率 |
|------|------|----------|
| poll | 3515 | 1.00× (基线) |
| epoll | 3060 | 0.87× |
| io_uring | 3003 | 0.85× |

**分析**: 在 32KB bulk 传输中，poll 仍然领先。epoll 和 io_uring 的额外开销源于更复杂的 event 注册/注销流程。在本地 Pipe 这种高吞吐场景，最简单的 poll 是最快的。io_uring 的优势在**网络 I/O**（accept/connect/read/write 的批量提交）和**真正异步**（无需注册/注销）场景中体现。

### 3.3 SHM 延迟 (epoll + eventfd)

| 引擎 | 延迟 (ms) |
|------|----------|
| epoll | 0.032 |

**分析**: SHM 延迟比 Pipe 高约 8 倍（0.032ms vs 0.004ms），这是预期的——SHM 需要跨进程共享内存写入 + eventfd 通知 + epoll 唤醒 + 数据读取，比 Pipe 的 kernel 内路径长。

### 3.4 多通道 (4 Pipes, epoll)

| 场景 | 延迟 (ms) |
|------|----------|
| 4 通道顺序激活（epoll） | 0.012 |

**分析**: 多通道场景下 epoll 的 O(1) 事件分发优势明显。在 4 个通道中，epoll 只需一次 `epoll_wait()` 即可返回就绪的通道。

## 4. 引擎选择建议

| 场景 | 推荐引擎 | 原因 |
|------|----------|------|
| 少量 Pipe/SHM 通道（< 10） | poll | 最小 syscall 开销 |
| 大量通道（10+） | epoll | O(1) 事件分发，monitor 模式 |
| 网络 I/O + 大量并发 | io_uring | 批量提交，零拷贝，真正异步 |
| 默认（AUTO） | epoll → poll | epoll 优先，回退 poll |

### 为什么 poll 在本地 Pipe 场景最快？

1. **单次 syscall**: `poll()` 一次调用完成注册+等待+注销
2. **epoll 开销**: 需要 `epoll_ctl(ADD)` + `epoll_wait()` + `epoll_ctl(DEL)` 三次 syscall
3. **io_uring 开销**: 需要 `io_uring_enter` 提交 SQE + `io_uring_enter` 等待 CQE + `io_uring_enter` 消费 POLL_REMOVE CQE

在本地 Pipe 场景，数据几乎立即可用，额外的注册/注销 syscall 开销大于收益。

## 5. io_uring 修复记录

### Bug: io_uring wait() 第 2 次迭代失败

**根因**: `uring_unwatch()` 提交 `IORING_OP_POLL_REMOVE` 后没有立即消费其 CQE。POLL_REMOVE 的 CQE 残留在完成队列中，被下一次 `wait()` 调用误认为是数据就绪事件。

**修复** (commit pending):
1. `uring_unwatch()` 中 POLL_REMOVE 使用 `user_data=0` 作为哨兵值
2. `uring_unwatch()` 提交后立即 `io_uring_enter()` 并 draining CQEs
3. `uring_wait()` 的 `process_cqe` 路径跳过 `user_data==0` 的 completion

## 6. 关联文档

- [02-async-io-phases.md](02-async-io-phases.md) — 异步 I/O 设计文档
- [04-performance.md](04-performance.md) — 性能优化计划
- `tests/test_aio_perf.c` — 基准测试代码
- `src/aio_uring.c` — io_uring 引擎实现
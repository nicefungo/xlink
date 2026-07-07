# 竞品对比分析：xlink vs nanomsg/nng vs ZeroMQ

> 创建: 2026-07-07 | Round 97 research
> 目的：了解 xlink 在同定位 C 通信库中的位置，识别差异化方向和改进空间

## 1. 同定位库概览

| 库 | 语言 | 代码量（core） | 依赖 | 许可证 | 定位 |
|----|------|---------------|------|--------|------|
| **xlink** | C99 | ~3,800 行 src | shm_ipc（内置） | 自用 | 进程间/网络通信基础层 |
| **nanomsg** | C | ~15,000 行 | 无 | MIT | 轻量级 SP 协议库 |
| **nng** | C11 | ~80,000 行 | 无（有 TLS/mbedTLS 可选） | MIT | nanomsg 继任者，生产级 |
| **ZeroMQ** | C++ | ~50,000 行 | libzmq | LGPL | 工业标准消息队列库 |

## 2. 功能对比矩阵

| 功能 | xlink | nanomsg | nng | ZeroMQ |
|------|-------|---------|-----|--------|
| **传输层** |
| TCP | ✅ | ✅ | ✅ | ✅ |
| UDP | ✅ | ❌ | ❌ | ❌（需额外协议） |
| Pipe (FIFO) | ✅ | ❌ | ❌ | ❌ |
| SHM | ✅ | ❌ | ❌ | ❌ |
| Serial | ✅ | ❌ | ❌ | ❌ |
| File I/O | ✅ | ❌ | ❌ | ❌ |
| IPC (Unix domain) | ❌ | ✅ | ✅ | ✅ |
| WebSocket | ❌ | ❌ | ✅ | ✅ |
| Inproc (线程间) | ❌ | ✅ | ✅ | ✅ |
| ZeroTier | ❌ | ❌ | ✅ | ❌ |
| **消息模式** |
| 帧协议（len-prefix） | ✅ | ❌（raw msg） | ❌（raw msg） | ❌（raw msg） |
| Pub/Sub | ❌ | ✅ | ✅ | ✅ |
| Req/Rep | ❌ | ✅ | ✅ | ✅ |
| Push/Pull | ❌ | ✅ | ✅ | ✅ |
| Pair（双向） | ✅（TCP/Pipe） | ✅ | ✅ | ✅ |
| Survey | ❌ | ✅ | ✅ | ❌ |
| Bus（mesh） | ❌ | ❌ | ✅ | ❌ |
| **异步 I/O** |
| epoll | ✅ | ❌ | ✅（内置 aio） | ✅（内置 aio） |
| poll | ✅ | ❌ | ✅ | ✅ |
| io_uring | ✅ | ❌ | ❌ | ❌ |
| 事件回调 `xlink_run()` | ✅ | ❌ | ✅（aio框架） | ✅（poller） |
| **安全** |
| TLS | ✅（OpenSSL 可选） | ❌ | ✅（1.2/1.3） | ✅（CURVE） |
| **扩展性** |
| 插件体系 | ✅（.so 动态加载） | ❌ | ✅（transport new API） | ❌（编译时） |

## 3. 性能特征对比

### xlink 实测数据（Linux 5.15, GCC -O2）

| 场景 | xlink | 预计 nng | 预计 ZMQ |
|------|-------|----------|----------|
| Pipe latency (1KB RTT) | **0.004ms** (poll) | ~0.01-0.02ms | ~0.03-0.05ms |
| Pipe throughput (32KB) | **3,515 MB/s** (poll) | ~1,500-2,500 MB/s | ~1,000-2,000 MB/s |
| SHM latency | 0.032ms | N/A | N/A |
| 多通道 (4 pipes) | 0.012ms | ~0.02ms | ~0.03ms |

> nng/ZMQ 预估值基于社区报告和架构推断。nng/ZMQ 的额外开销来自：
> - 消息模式层（pub/sub 路由、filtering）
> - 内置线程池/事件循环
> - 消息分配/释放（zmq_msg_t 堆分配）
> 
> xlink 在裸 Pipe/TCP 场景更快，因为它没有这些上层抽象。

### xlink 的性能优势来源

1. **无消息模式层** — xlink 是纯粹传输层，没有 pub/sub 路由、消息队列、filtering
2. **帧协议极简** — 4 字节长度前缀，无连接握手、无 backpressure 协商
3. **无堆分配在热路径** — `xlink_send()`/`xlink_recv()` 使用调用者提供的缓冲区
4. **SHM + eventfd** — 跨进程共享内存是 nng/ZMQ 的 IPC 传输无法匹敌的（后者仍然走 kernel）
5. **io_uring** — 唯一在同一体量下支持 io_uring 的 C 通信库

### xlink 的劣势

1. **缺少消息模式** — 如果用户需要 pub/sub，需要自己在应用层实现
2. **缺少连接管理** — 对端断开后需要用户处理重连逻辑（TCP 有 auto-reconnect，但无指数退避等）
3. **缺少线程安全保证** — 多线程操作同一 channel 未定义行为
4. **缺少序列化** — 消息是裸字节，没有内置的多帧、envelope 等

## 4. 差异化方向总结

| 方向 | xlink 优势 | 意义 |
|------|-----------|------|
| **传输层多样性** | SHM、Pipe、Serial、File、UDP — nng/ZMQ 不支持 | 嵌入式/设备场景独有 |
| **裸性能** | 0.004ms Pipe latency、3.5 GB/s throughput | 高频数据管道最佳选择 |
| **io_uring** | 唯一支持者 | Linux 5.1+ 的高并发场景 |
| **零依赖基本构建** | 仅 .a 文件、单头文件 | 易于嵌入/vendor |
| **SHM + eventfd** | 跨进程零拷贝通信 | 本地进程间最快路径 |
| **帧协议** | 自动分帧/合帧 | 用户不需要处理 TCP 粘包 |

## 5. 建议方向（供 future-plans 参考）

### 短期（v2.2+）
- **IPC (Unix domain socket) 支持** — 填补 Pipe 和 TCP 之间的性能/功能空白
- **消息模式层**（可选） — 在帧协议之上提供 pub/sub、req/rep 语义
- **连接池和健康检查** — 减少 auto-reconnect 的盲目性

### 中期（v3.0）
- **线程安全** — 每个 channel 的内部锁，允许多线程 send/recv
- **WebSocket 传输** — 浏览器/Web 集成
- **序列化格式** — flatbuffers/protobuf 可选集成

### 长期
- **跨平台** — 将 SHM 语义映射到 Windows/macOS 等价实现
- **PAL 层** — 从现有代码抽取平台相关调用，统一为内部抽象

## 6. 关联文档

- [06-perf-benchmarks.md](06-perf-benchmarks.md) — xlink 基准测试数据
- [04-performance.md](04-performance.md) — 性能优化计划
- [05-multi-platform.md](05-multi-platform.md) — 跨平台计划（PAL 层）
- [03-tls-security.md](03-tls-security.md) — TLS 加密计划

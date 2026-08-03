# 跨平台支持

## 动机

xlink 当前仅支持 Linux。在以下场景下，跨平台支持是刚需：

- **嵌入式 RTOS**：FreeRTOS、Zephyr 等（设备端）
- **Windows**：桌面工具链、WSL 集成
- **macOS**：开发环境的本地测试
- **Android/iOS**：移动端通信

## 设计方案

### 平台抽象层（PAL）

```c
// include/xlink_pal.h — 平台抽象接口

// 线程/同步（非核心，但为插件化预留）
typedef struct xlink_mutex xlink_mutex_t;
typedef struct xlink_cond  xlink_cond_t;
typedef struct xlink_thread xlink_thread_t;

// 文件 I/O
int  xlink_pal_open(const char *path, int flags, int mode);
int  xlink_pal_close(int fd);
ssize_t xlink_pal_read(int fd, void *buf, size_t n);
ssize_t xlink_pal_write(int fd, const void *buf, size_t n);
int  xlink_pal_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

// 共享内存
int  xlink_pal_shm_open(const char *name, int oflag, mode_t mode);
int  xlink_pal_shm_unlink(const char *name);
void *xlink_pal_mmap(void *addr, size_t len, int prot,
                     int flags, int fd, off_t off);
int  xlink_pal_munmap(void *addr, size_t len);

// 套接字
int  xlink_pal_socket(int domain, int type, int protocol);
int  xlink_pal_connect(int fd, const struct sockaddr *addr, socklen_t len);
int  xlink_pal_bind(int fd, const struct sockaddr *addr, socklen_t len);
int  xlink_pal_listen(int fd, int backlog);
int  xlink_pal_accept(int fd, struct sockaddr *addr, socklen_t *len);

// 时间/睡眠
uint64_t xlink_pal_now_ms(void);
int xlink_pal_usleep(useconds_t usec);

// 错误码（平台无关化）
int xlink_pal_errno(void);
const char *xlink_pal_strerror(int err);
```

### 平台支持矩阵

| 功能 | Linux | Windows | macOS | FreeRTOS |
|------|-------|---------|-------|----------|
| TCP    | ✅ native | ✅ winsock2 | ✅ native | ⚠️ LwIP |
| UDP    | ✅ native | ✅ winsock2 | ✅ native | ⚠️ LwIP |
| SHM    | ✅ mmap | ⚠️ CreateFileMapping | ⚠️ mmap | ❌ |
| File   | ✅ | ✅ | ✅ | ⚠️ 可能需要 VFS |
| Serial | ✅ | ✅ | ✅ | ⚠️ UART |
| Pipe   | ✅ | ✅ | ✅ | ❌ |
| IPC (AF_UNIX) | ✅ (src/ipc_backend.c, 2026-08) | ⚠️ 命名管道 | ⚠️ AF_UNIX | ❌ |
| io_uring | ✅ | ❌ | ❌ | ❌ |
| TLS    | ✅ OpenSSL | ✅ SChannel | ✅ SecureTransport | ⚠️ mbedTLS |

## 实现路径

### Phase 1: POSIX 平台统一（macOS + BSD）

- [ ] 创建 `xlink_pal.h`，从现有代码抽取平台相关调用
- [ ] macOS 适配（没有 `memfd_create`、`eventfd`、`signalfd`——用 pipe 替代）
- [ ] 测试：macOS 上所有测试通过
- [ ] CI: 添加 macOS runner

### Phase 2: Windows 支持

- [ ] winsock2 适配
- [ ] **Windows 命名管道替代 AF_UNIX（IPC 后端）** — 见下方「AF_UNIX → 命名管道映射」
- [ ] `CreateFileMapping` 替代 `shm_open`
- [ ] Windows IOCP 替代 poll（可选）
- [ ] MSVC / MinGW 构建支持（CMake）
- [ ] 测试：Windows 上关键功能测试
- [ ] CI: 添加 Windows runner

### AF_UNIX → Windows 命名管道映射（2026-08 增补）

2026-08-02 新增的 `src/ipc_backend.c`（XLINK_IPC，AF_UNIX SOCK_STREAM，`ipc://` 或裸路径）是
本地进程间通信最轻量的后端（~30% 快于 localhost TCP，绕过网络栈）。Windows 上与其语义
最接近的等价物是 **命名管道 (Named Pipes)**，映射关系如下：

| AF_UNIX (Linux) | Windows Named Pipe | 说明 |
|-----------------|--------------------|------|
| `socket(AF_UNIX, SOCK_STREAM)` | `CreateNamedPipe` (PIPE_TYPE_BYTE) | 面向字节流，语义对应 SOCK_STREAM |
| `bind()` 到路径 | `\\.\pipe\<name>` 命名 | IPC 地址天然是字符串，可直接复用 `ipc://` URL 的 path 部分 |
| `listen()` + `accept()` | `ConnectNamedPipe` (server) | 服务器端阻塞等待客户端连接 |
| `connect()` | `CreateFile` + `WaitNamedPipe` | 客户端连接（未就绪时可等待） |
| `send()`/`recv()` | `WriteFile`/`ReadFile` (OVERLAPPED) | 用 overlapped I/O 对接现有 poll/事件循环 |
| `unlink()` 清理 socket 文件 | 进程退出时管道自动消失 | Windows 无需显式清理，生命周期更简单 |
| `poll()` 可读/可写事件 | 管道句柄 + `WaitForMultipleObjects` / IOCP | 事件模型需适配，无法直接复用 `xlink_poll` |

**适配要点**：
- `ipc_backend.c` 的 `create`/`connect`/`send`/`recv`/`close` vtable 五个入口是平台无关的，
  只需在 PAL 层替换底层 syscall 即可，后端上层逻辑（framer、消息分帧、URL 解析）完全复用。
- URL 解析 `ipc:///path` 已在 `xlink.c` 完成，`/path` 直接映射为 Windows 的 `\\.\pipe\path`。
- 命名管道默认是**同步阻塞**的；要用 overlapped I/O 才能与 `xlink_wait_aio()` 事件循环集成，
  这对应 Phase 2 中「Windows IOCP 替代 poll」的子任务。

### Phase 3: 嵌入式 RTOS

- [ ] FreeRTOS + LwIP 适配
- [ ] 无动态内存分配模式（所有缓冲区编译时分配）
- [ ] 最小配置（只编译需要的后端）
- [ ] 测试：QEMU 模拟环境

## 依赖

- **Phase 1**: 无额外依赖（macOS 核心是 POSIX）
- **Phase 2**: CMake（Windows 构建）
- **Phase 3**: FreeRTOS + LwIP 源码
- **前置依赖**: [插件化架构](01-plugins-arch.md)（后端管理接口统一后，平台适配更容易）

## 开放问题

1. **代码组织结构**: PAL 层代码放在 `src/pal/` 目录？还是每个平台独立目录 `src/linux/`, `src/win32/`？
2. **Makefile vs CMake**: 跨平台构建是否必须迁移到 CMake？当前 Makefile + wildcard 模式是否足够？
3. **功能子集**: 嵌入式平台可能只支持 TCP/Serial 子集。PAL 应该如何定义"最小必需实现"？
4. **测试策略**: 如何在不拥有所有平台硬件的情况下进行跨平台测试？QEMU 模拟 vs CI 设备 farm？

## 关联文档

- [插件化架构](01-plugins-arch.md) — 跨平台是插件化的关键驱动场景
- [异步 I/O 支持](02-async-io.md) — io_uring 仅 Linux，其他平台需要不同的异步方案
- [TLS 加密通信层](03-tls-security.md) — 各平台需不同的 TLS 库适配
- [性能优化](04-performance.md) — MSG_ZEROCOPY、splice() 等 Linux 特有优化

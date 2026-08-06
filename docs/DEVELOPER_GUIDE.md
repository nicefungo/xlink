# xlink 开发者指南（DEVELOPER GUIDE）

> 版本：基于当前 master（2026-08-06）| 对象：任何将要接手/贡献本仓库的开发者
> 本文档是**上手指南**，回答「这项目是什么、怎么跑、怎么改、怎么测、怎么提交」。
> 架构深挖见 `code-walkthrough.md`，API 全量见 `api.md`。

---

## 1. 这是什么

xlink 是一个**嵌入式级 C 跨应用/跨设备通信库**。核心理念：**一种 API，多种传输**。
你写一次 `open/send/recv/close`，换一个地址字符串，就能在共享内存、管道、TCP、UDP、
串口、文件、Unix 域套接字之间无缝切换。

```c
#include "xlink.h"
xlink_opt_t opt = XLINK_OPT_DEFAULT;
opt.flags = XLINK_CREATE;
xlink_channel_t* ch = xlink_open(XLINK_SHM, "/chan", &opt); // 换地址即换传输
xlink_send(ch, data, len);
xlink_recv(ch, buf, &len);
xlink_close(ch);
```

### 当前状态（2026-08-06 快照）

- **7 个内置后端**：`shm` `pipe` `tcp` `udp` `serial` `file` `ipc`（RTSP 枚举保留未实现）
- **构建**：`make all` → **0 warnings**（`-Wall -Wextra -O2 -g`）
- **测试**：约 **49 个测试套件**，`make test` 全绿
- **版本**：v2.x（插件化 + 异步 I/O + TLS + 零拷贝 + 批量化 + 无锁队列已完成）

---

## 2. 环境与构建

需要：`gcc`、`make`、POSIX 基础库（glibc/pthread/librt）。TLS 另需 OpenSSL 开发头。

```sh
cd ~/xlink
make all      # 编译 lib + tools + tests
make test     # 跑全部测试（全绿 = 健康）
make tls      # 启用 XLINK_HAS_TLS 重编（含 TLS 测试）
make stress   # 压力测试
make clean    # 清理
```

> ⚠️ **构建依赖**：`third_party/shm_ipc` 是指向 `~/shm_ipc` 的符号链接。
> 换机器后先确认它存在，否则 SHM 后端编不过。

---

## 3. 目录结构速查

```
include/xlink.h      ← 公共 API（唯一权威接口定义，改 API 必须动这里）
include/spsc_queue.h · mpsc_queue.h   ← 无锁队列内部头
src/
  xlink.c            ← 中央调度 + framing(4B 大端长度前缀) + 公共 API 实现
  xlink_internal.h   ← 后端 vtable(xlink_backend_t) + channel 结构
  plugin.c           ← 插件注册表 + 7 个内置后端注册 + xlink_open_url 协议识别
  aio.c aio_epoll.c aio_poll.c aio_uring.c   ← 异步 I/O 引擎（poll/epoll/io_uring）
  tls.c              ← OpenSSL 封装（证书、SNI、ALPN）
  {shm,pipe,tcp,udp,serial,file,ipc}_backend.c  ← 各传输后端
  spsc_queue.c mpsc_queue.c          ← 无锁队列实现
tools/               ← CLI：send / recv / bridge / monitor
tests/               ← 每后端一个/多个测试 + 压力 + 零拷贝 + TLS
docs/                ← 文档（本文件所在目录）
third_party/shm_ipc  → symlink to ~/shm_ipc
```

---

## 4. 核心架构：后端 vtable

一切以 **`xlink_backend_t`**（在 `src/xlink_internal.h`）为中心。每个传输实现一套钩子，
`xlink.c` 只调 vtable，不关心具体传输：

```c
typedef struct xlink_backend {
    const char *name;                       /* "shm" / "tcp" / ... */
    xlink_type_t type;                      /* XLINK_SHM / ... */
    int  (*open)(channel*, ...);            /* 打开/绑定/连接 */
    int  (*send)(channel*, const void*, size_t);
    int  (*recv)(channel*, void*, size_t*);
    int  (*write)(channel*, ...);           /* 低层流式写（绕过 framing） */
    int  (*read)(channel*, ..., int timeout_ms);
    int  (*close)(channel*);
    /* 零拷贝钩子 */
    int  (*send_zc)(...);  int (*recv_zc)(...);
    void (*recv_zc_done)(...); int (*zc_capable)(...);
} xlink_backend_t;
```

**新增一个后端 = 实现这套 vtable + 在 `plugin.c` 的 `builtin_plugins[]` 注册。** 无需改 `xlink.c`。

> 内置后端在 `plugin.c` 的 `xlink_plugins_init()` 首次使用时懒初始化注册到哈希表。

---

## 5. 关键子系统（按重要性）

### 5.1 Framing（xlink.c）
- **流式传输**（PIPE/TCP/SERIAL/IPC）：每个消息前加 **4 字节大端长度前缀**。
- **数据报传输**（SHM/UDP/File）：不加帧，直通，效率优先。
- 超大消息静默丢弃（设计决策，见 `design-decisions.md`）。

### 5.2 插件系统（plugin.c）
- `xlink_plugin_load(".so")` 加载动态库，须导出符号 `xlink_plugin_export`（类型 `xlink_plugin_t`）。
- `xlink_open_url("tcp://...")` 按 scheme 自动路由到对应插件/后端。

### 5.3 异步 I/O（aio*.c）
- `xlink_aio_create(type)`：`0=AUTO 1=POLL 2=EPOLL 3=IO_URING`，AUTO 按平台选。
- `xlink_wait_aio(chans, n, timeout, engine)`：事件驱动多通道等待。
- `xlink_run(chans, n, timeout, engine, cb, arg)`：事件驱动主循环。

### 5.4 TLS（tls.c，编译开关 `XLINK_HAS_TLS`）
- `xlink_tls_configure()` 在 `open` 之后、首次收发之前调用。
- 支持：证书校验（`verify_peer`/`ca_file`）、SNI（`sni_hostname`）、**ALPN**（`alpn_protos` → `alpn_negotiated`，通过 `xlink_tls_alpn_negotiated()` 读取）。

### 5.5 零拷贝（ZC）
- `xlink_send_zc()`（异步，所有权转移给内核，用回调或 `xlink_zc_poll()` 等完成）。
- `xlink_recv_zc()`（返回指向共享/内部内存的指针，用完 `xlink_recv_zc_done()` 释放）。
- 后端支持：SHM（指针传递）、TCP（`MSG_ZEROCOPY`）、File（`copy_file_range`）。
- `xlink_zc_notify_fd()` 返回可 epoll 的 eventfd，完成时变可读。

### 5.6 批量化 + 无锁队列
- `xlink_send_batch()` / `xlink_recv_batch()`：一次调用多消息。
- `xlink_set_batch_policy()`：自适应批量化（EWMA 速率检测动态调批大小）。
- `xlink_lfq_init()` / `xlink_lfq_flush()`：SHM 锁自由 SPSC 发送队列。

### 5.7 自动重连
- TCP / IPC 客户端自动重连：指数退避（线性增长至 1600ms 封顶），`sendmsg(MSG_NOSIGNAL)` 防 SIGPIPE。

---

## 6. 测试

- 测试在 `tests/test_*.c`，每个编译成独立二进制进 `bin/tests/`。
- **Makefile 用 wildcard 自动发现**：新增 `tests/test_foo.c` 无需改 Makefile。
- 运行：`make test`（串行，因测试端口硬编码）。
- 写法：一般 `fork()` 出对端（server/client 或 sender/receiver），断言 PASS/FAIL，最后打印 `=== RESULTS: N checks, 0 failures ===`。

**新增功能请务必补测试**，覆盖错误路径与边界值。提交前跑 `make test` 全绿。

---

## 7. 提交规范（重要）

```sh
cd ~/xlink && git add -A && git commit -m "类型: 简述"
git push origin master
```

- **commit message 必含日期**：`daily: 2026-08-06 - 做了什么`
- 类型前缀：`feat:` `fix:` `perf:` `docs:` `daily:`
- 从 `master` 分支推送到 `origin`（GitHub: nicefungo/xlink）。

---

## 8. 改代码前必读

1. **先读 `include/xlink.h`** —— 一切公共接口的权威。改 API 先在这里改签名。
2. **再读 `src/xlink_internal.h`** —— 后端 vtable 和 channel 结构。
3. **对照 `docs/code-walkthrough.md`** —— 逐模块导读，避免重复造轮子。
4. **遵循「Less is More」**：代码越少 bug 越少。能 20 行解决别写 200 行。
5. **改动要外科手术式**：只改必要的行，不顺手重构无关代码。
6. **跑测试**：`make all`（0 warnings）+ `make test`（全绿）再提交。

---

## 9. 常见坑

- **SHM 段残留**：进程被 `kill -9` 时 `atexit` 清理不执行，SHM 段残留。用 `ipcrm -M` 或 monitor 工具清理。
- **测试端口硬编码**：`19897`（overflow）、`19992`（frame_overflow）等。测试必须**串行**跑。
- **Serial 波特率**：未知波特率静默回退 9600。写死已知值。
- **TLS 是可选编译**：不开 `XLINK_HAS_TLS` 时 TLS 相关声明不编译。
- **third_party/shm_ipc**：符号链接，别提交成实体目录。

---

## 10. 文档导航

| 文档 | 用途 |
|------|------|
| **本文件（DEVELOPER_GUIDE.md）** | 新贡献者上手 |
| `api.md` | 完整 API 参考 |
| `code-walkthrough.md` | 逐模块源码导读 |
| `design-decisions.md` | 关键架构决策与权衡 |
| `known-issues.md` | 已知问题 / design-by-choice |
| `future-plans/` | 路线图与未做规划 |
| `proposal.md` / `technical-report.md` | 早期设计 / v1.0 技术报告 |

# xlink — Cross-App Communication Toolkit

统一 API 的跨应用/跨设备通信工具箱。一个 `open/send/recv/close` 通吃共享内存、管道、TCP、UDP、串口、文件与 Unix 域套接字（IPC）。

当前版本：**v2.x**（插件化 + 异步 I/O + TLS + 零拷贝 + 批量化 + 无锁队列，均已完成并合入 master）。

## 支持的传输

| 传输 | 地址格式 | 状态 | 后端 |
|------|---------|------|------|
| SHM | `/my_channel` | ✅ | `src/shm_backend.c` |
| Pipe | `/tmp/xlink.pipe` | ✅ | `src/pipe_backend.c` |
| TCP | `host:port` / `:port` | ✅ | `src/tcp_backend.c` |
| UDP | `host:port` / `group:port` | ✅ | `src/udp_backend.c` |
| Serial | `/dev/ttyX[:baud]` | ✅ | `src/serial_backend.c` |
| File | `/tmp/record.bin` | ✅ | `src/file_backend.c` |
| IPC (AF_UNIX) | `/path/to/sock` / `ipc:///path` / `.sock:/path` | ✅ | `src/ipc_backend.c` |
| RTSP | `rtsp://...` | 📋 保留枚举，未实现 | — |

## 核心特性（当前版本）

- **统一的 open/send/recv/close API** — 换地址即可切换传输方式，代码零改动
- **插件系统** — `xlink_plugin_load(".so")` 动态加载，`xlink_open_url()` 按 scheme 自动识别协议
- **异步 I/O 引擎** — `xlink_aio_create()` / `xlink_wait_aio()`，支持 poll / epoll / io_uring 三种引擎（AUTO 自动选择）；`xlink_run()` 事件驱动主循环
- **TLS 加密**（`XLINK_HAS_TLS` 编译开关）— `xlink_tls_configure()`，支持证书校验、SNI、**ALPN 协议协商**
- **零拷贝传输** — `xlink_send_zc()` / `xlink_recv_zc()`，SHM（指针传递）、TCP（MSG_ZEROCOPY）、File（copy_file_range）三后端实现；eventfd 完成通知 `xlink_zc_notify_fd()`
- **批量化** — `xlink_send_batch()` / `xlink_recv_batch()`，自适应批量化策略 `xlink_set_batch_policy()`
- **无锁队列** — SHM 后端锁自由 SPSC/MPSC 队列（`xlink_lfq_init()` / `xlink_lfq_flush()`）
- **多通道等待** — `xlink_wait()`（poll 版）与 `xlink_wait_aio()`（事件驱动版）
- **TCP/IPC 自动重连** — 指数退避 + `MSG_NOSIGNAL` 防 SIGPIPE 杀进程

## 目录结构

```
xlink/
├── README.md
├── Makefile            ← make all / make test / make clean / make tls / make stress
├── include/xlink.h     ← 公共 API（唯一权威接口定义）
├── include/            ← 内部无锁队列头（spsc_queue.h / mpsc_queue.h）
├── src/
│   ├── xlink.c         ← 中央调度 + framing 层 + 公共 API 实现
│   ├── xlink_internal.h ← 后端 vtable + channel 结构定义
│   ├── plugin.c        ← 插件注册表 + 内置后端注册
│   ├── aio.c / aio_epoll.c / aio_poll.c / aio_uring.c ← 异步引擎
│   ├── tls.c           ← TLS (OpenSSL) 封装
│   ├── shm_backend.c   ← SHM（支持锁自由 SPSC）
│   ├── pipe_backend.c / tcp_backend.c / udp_backend.c
│   ├── serial_backend.c / file_backend.c / ipc_backend.c
│   └── spsc_queue.c / mpsc_queue.c ← 无锁队列
├── tools/              ← CLI 工具（send / recv / bridge / monitor）
├── tests/              ← 测试（每后端独立测试 + 压力 + 零拷贝 + TLS）
├── docs/               ← 设计文档 / API 参考 / 开发者指南
└── third_party/shm_ipc → 符号链接到 ~/shm_ipc（SHM 底层）
```

## 快速开始

```sh
cd ~/xlink && make all

# 写文件 + 读文件
bin/tools/send --create file /tmp/test.msg   # 从 stdin 写入
bin/tools/recv file /tmp/test.msg             # 读出到 stdout

# SHM（需要先打开接收端再发送）
bin/tools/recv shm /my_channel                # 终端 1：先收
bin/tools/send --create shm /my_channel       # 终端 2：再发

# TCP
bin/tools/recv --server tcp :8080             # 终端 1：服务端监听
bin/tools/send tcp 127.0.0.1:8080             # 终端 2：客户端发送

# IPC（AF_UNIX，本机进程间通信，比 TCP 快 ~30%）
bin/tools/recv --server ipc ipc:///tmp/x.sock # 终端 1：服务端
bin/tools/send ipc ipc:///tmp/x.sock          # 终端 2：客户端

# 桥接（任意两个传输之间透明转发）
bin/tools/bridge serial /dev/ttyUSB0 udp :5555    # 串口→UDP 组播
bin/tools/bridge shm /my_channel tcp 192.168.1.5:8080

# 监控
bin/tools/monitor udp :5555
```

## 构建

```sh
make all     # 编译全部（lib + tools + tests），0 warnings (-Wall -Wextra)
make test    # 运行全部测试（49 套件，全通过即为健康状态）
make tls     # 启用 XLINK_HAS_TLS 重新编译（含 TLS 测试）
make stress  # 压力测试
make clean   # 清理构建产物
```

依赖：`gcc` / `make` / POSIX 基础库（所有 Linux 发行版预装）。TLS 需要 OpenSSL 开发头文件。

## API 快速参考

```c
#include "xlink.h"

xlink_opt_t opt = XLINK_OPT_DEFAULT;
opt.flags = XLINK_CREATE;

xlink_channel_t* ch = xlink_open(XLINK_SHM, "/channel", &opt);
xlink_send(ch, data, len);
xlink_recv(ch, buf, &len);
xlink_close(ch);
```

详细 API 文档见 `docs/api.md`。给新贡献者的完整指南见 **`docs/DEVELOPER_GUIDE.md`**。

## 设计要点

- **零外部依赖（核心）** — 只依赖 glibc + pthread + librt；TLS 为可选（`XLINK_HAS_TLS`）
- **流式传输自动加帧** — PIPE/TCP/SERIAL/IPC 自动加 4 字节大端长度前缀
- **数据报传输直通** — SHM/UDP/File 不加帧，效率优先
- **后端 vtable + 插件注册表** — 7 个内置后端注册在 `plugin.c` 的 static 数组中，可动态加载第三方 `.so`
- **shm_ipc 复用** — SHM 后端直接封装现有库；锁自由 SPSC 可绕过其内部锁
- **向后兼容** — 新特性全部以新增 API 形式加入，`xlink_wait()` 等旧接口保持不变

## 谁在用 / 文档导航

- `docs/api.md` — 完整 API 参考
- `docs/DEVELOPER_GUIDE.md` — **新贡献者从这里开始**（架构、构建、测试、贡献规范）
- `docs/code-walkthrough.md` — 逐模块源码导读
- `docs/design-decisions.md` — 关键架构决策与权衡
- `docs/known-issues.md` — 已知问题与 design-by-choice
- `docs/future-plans/` — 路线图与未来规划

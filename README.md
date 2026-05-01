# xlink — Cross-App Communication Toolkit

统一 API 的跨应用/跨设备通信工具箱。一个 `open/send/recv/close` 通吃共享内存、管道、TCP、UDP、串口、文件。

## 支持的传输

| 传输 | 地址格式 | 状态 | 测试 |
|------|---------|------|------|
| SHM | `/my_channel` | ✅ | 10000 msg @ 72K msg/s |
| Pipe | `/tmp/xlink.pipe` | ✅ | round-trip + writer/reader |
| TCP | `host:port` / `:port` | ✅ | fork-based server/client |
| UDP | `host:port` / `:port` | ✅ | fork-based sender/receiver |
| Serial | `/dev/ttyX[:baud]` | ✅ | PTY loopback, 8N1 |
| File | `/tmp/record.bin` | ✅ | record + replay |
| RTSP | `rtsp://...` | 📋 | future work |

## 目录结构

```
xlink/
├── README.md
├── Makefile            ← make all / make test / make clean
├── include/xlink.h     ← 公共 API
├── src/
│   ├── xlink.c         ← 中央调度 + 4 字节 framing 层
│   ├── xlink_internal.h ← 后端 vtable + channel 结构定义
│   ├── shm_backend.c   ← SHM（封装 shm_ipc）
│   ├── pipe_backend.c  ← 命名管道 FIFO
│   ├── tcp_backend.c   ← TCP 客户端/服务端
│   ├── udp_backend.c   ← UDP 单播/组播
│   ├── serial_backend.c← RS-232/485 串口
│   └── file_backend.c  ← 文件录制/回放
├── tools/              ← CLI 工具
│   ├── send.c          → bin/tools/send
│   ├── recv.c          → bin/tools/recv
│   ├── bridge.c        → bin/tools/bridge
│   └── monitor.c       → bin/tools/monitor
├── tests/              ← 测试（每个后端一个独立测试 + 压力测试）
├── docs/               ← 设计文档 / API 参考
└── third_party/shm_ipc → 符号链接到 ~/shm_ipc
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

# 桥接（任意两个传输之间透明转发）
bin/tools/bridge serial /dev/ttyUSB0 udp :5555    # 串口→UDP 组播
bin/tools/bridge shm /my_channel tcp 192.168.1.5:8080

# 监控
bin/tools/monitor udp :5555
```

## 构建

```sh
make all     # 编译全部（lib + tools + tests）
make test    # 运行全部测试（全通过即为健康状态）
make clean   # 清理构建产物
```

依赖：`gcc` / `make` / POSIX 基础库（所有 Linux 发行版预装）。

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

详细 API 文档见 `docs/api.md`。

## 设计要点

- **零外部依赖** — 只依赖 glibc + pthread + librt
- **流式传输自动加帧** — PIPE/TCP/SERIAL 自动加 4 字节大端长度前缀
- **数据报传输直通** — SHM/UDP/File 不加帧，效率优先
- **后端 vtable** — 6 个后端各实现 7 个钩子，注册在 static 数组中
- **shm_ipc 复用** — SHM 后端直接封装现有库，不改一行

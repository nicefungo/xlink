# xlink 模块接入手册

第三方程 / 外部应用 / 插件模块如何与 xlink 通信。

---

## 目录

1. [概述](#1-概述)
2. [方案 A：链接 libxlink（推荐）](#2-方案-a链接-libxlink推荐)
3. [方案 B：实现 Wire Protocol](#3-方案-b实现-wire-protocol)
4. [Wire Protocol 规范](#4-wire-protocol-规范)
5. [按传输层的对接要求](#5-按传输层的对接要求)
6. [地址约定（Rendezvous）](#6-地址约定rendezvous)
7. [绑定 / 多语言支持](#7-绑定--多语言支持)
8. [调试与验证](#8-调试与验证)
9. [常见问题](#9-常见问题)

---

## 1. 概述

xlink 本质是一个 **多传输消息总线**。你写一段 C 代码、一个 Python 脚本、甚至一个 shell 管道，都可以跟 xlink 互发消息。

通信模型：

```
App A ──[xlink API]──→ 传输层 ──[Wire Protocol]──→ 传输层 ──[xlink API]──→ App B
```

接入方式分两个层级：

| 层级 | 条件 | 推荐 |
|------|------|------|
| A. 链接 libxlink.a | 模块可以用 C（或 FFI） | ✅ 首选 |
| B. 实现 Wire Protocol | 模块是其他语言 / 嵌入式 | ✅ 备选 |

---

## 2. 方案 A：链接 libxlink（推荐）

### 2.1 构建接入

编译时加上：

```makefile
CFLAGS  += -I<path-to-xlink>/include -I<path-to-xlink>/third_party/shm_ipc/include
LDFLAGS += -L<path-to-xlink>/bin -lxlink -L<path-to-xlink>/third_party/shm_ipc/bin -lshm_ipc -lpthread -lrt
```

或者直接链接静态库：

```sh
gcc -I./xlink/include my_module.c ./xlink/bin/libxlink.a -lrt -lpthread -o my_module
```

### 2.2 头文件

```c
#include "xlink.h"
```

所有类型（`xlink_channel_t*`）、所有常量（`XLINK_SHM`, `XLINK_CREATE` 等）都通过这一个头文件暴露。

### 2.3 必须实现的 API 调用

```c
/* 1. 打开通道 */
xlink_channel_t* ch = xlink_open(XLINK_PIPE, "/tmp/my_channel", &opt);

/* 2. 发送消息（阻塞） */
xlink_send(ch, data, len);

/* 3. 接收消息（阻塞 / 等待） */
size_t len = sizeof(buf);
xlink_recv(ch, buf, &len);

/* 4. 多路等待（同时监听多个通道） */
xlink_channel_t* chans[2] = { ch_a, ch_b };
int ready = xlink_wait(chans, 2, -1);  /* 返回有数据的索引 */

/* 5. 关闭 */
xlink_close(ch);
```

### 2.4 模块初始化与退出

**初始化** — 无特殊要求。第一次调用 `xlink_open` 不依赖任何全局状态。

**退出** — 调用 `xlink_close` 即可。如果模块用了 `XLINK_CREATE | XLINK_SHM` 创建 SHM 段，xlink 会在 `atexit` 时自动清理。不需要模块自己调 `shm_destroy`。

### 2.5 最小接入示例（C）

```c
#include "xlink.h"
#include <stdio.h>
#include <string.h>

int my_module_init(const char* peer_addr) {
    xlink_opt_t opt = XLINK_OPT_DEFAULT;
    xlink_channel_t* ch = xlink_open(XLINK_TCP, peer_addr, &opt);
    if (!ch) {
        fprintf(stderr, "xlink: cannot reach %s\n", peer_addr);
        return -1;
    }

    char buf[4096];
    size_t len;

    while (1) {
        len = sizeof(buf);
        if (xlink_recv(ch, buf, &len) != 0) break;
        /* ... process buf[0..len-1] ... */
    }

    xlink_close(ch);
    return 0;
}
```

### 2.6 v2.0 新功能（实验性）

v2.0 引入了两个新子系统，目前处于实现中状态：

#### 插件化加载（xlink_plugin_load）

```c
/* 从 .so 文件动态加载后端插件 */
int xlink_plugin_load(const char *so_path);

/* 查询当前注册的插件数量 */
size_t xlink_plugin_count(void);

/* 通过 URL 打开通道（自动路由到对应后端） */
xlink_channel_t* xlink_open_url(const char *url,
                                 const xlink_opt_t *opt);
```

URL 格式：`<scheme>://<path>`，例如 `"shm://mychan"` 或 `"tcp://server:8080"`。
插件需要导出 `xlink_plugin_export` 符号，与内置后端 API 版本匹配。

#### 异步 I/O 引擎（xlink_wait_aio）

```c
/* 创建异步 I/O 引擎 */
void *xlink_aio_create(int type);   /* 0=AUTO, 1=POLL, 2=EPOLL */

/* 销毁引擎 */
void  xlink_aio_destroy(void *engine);

/* 事件驱动版 wait */
int xlink_wait_aio(xlink_channel_t **chans, int n,
                   int timeout_ms, void *aio_engine);
```

引擎通过 `void*` 句柄暴露以实现 ABI 稳定性。当前实现支持 epoll（Linux 2.6+）
和 poll（POSIX 回退）。`xlink_aio_create(0)` 自动选择最佳可用引擎。

**注意**：这些 API 为实验性，接口可能在未来版本调整。

---

## 3. 方案 B：实现 Wire Protocol

如果模块**不能**直接链接 libxlink（比如用 Python、Rust、Go，或是嵌入式 MCU），你只需在传输层上实现 xlink 的**消息帧格式**。

要求非常简单：

- 建立传输层连接（见 [§5](#5-按传输层的对接要求)）
- 按 [§4](#4-wire-protocol-规范) 的帧格式读写消息
- 按 [§7](#7-绑定--多语言支持) 参考现成实现

---

## 4. Wire Protocol 规范

### 4.1 帧格式（仅限流式传输）

用于 **PIPE / TCP / SERIAL**（流式字节流）。

每一条消息的线缆格式：

```
+----------------+-------------------+
|  4-byte 长度   |  N-byte 载荷       |
|  (大端 uint32) |  (原始字节)       |
+----------------+-------------------+
```

```
字节 0–3：      载荷长度（网络字节序 / Big-Endian）
                 0x00000000 = 空消息（允许）
字节 4–(4+N-1)：载荷数据（原始二进制，无需编码）
```

**伪代码——发送端：**

```c
uint32_t netlen = htonl((uint32_t)payload_len);
write(fd, &netlen, 4);            /* 先写长度头 */
write(fd, payload, payload_len);  /* 再写载荷 */
```

**伪代码——接收端：**

```c
uint32_t netlen;
read_exact(fd, &netlen, 4);       /* 先读 4 字节长度头 */
uint32_t len = ntohl(netlen);
read_exact(fd, buf, len);         /* 再读载荷 */
```

### 4.2 非流式传输

**SHM / UDP / FILE**（消息天然有边界），**不使用帧格式**：

- SHM：每个 `shm_write` / `shm_readn` 调用即是一条完整消息
- UDP：每个 `sendto` / 每个 `recvfrom` 即是一条完整消息
- FILE：文件中每条记录是 `4 字节长度 + 载荷` 的序列（同帧格式，写串行）

对接 SHM 或 UDP 时，直接读写即可，无需额外处理。

### 4.3 最大消息大小

| 传输 | 最大载荷 | 说明 |
|------|---------|------|
| SHM  | 4096 bytes | 由 shm_ipc 底层限制 |
| PIPE | 无硬限制 | 由管道缓冲区容量决定 |
| TCP  | 无硬限制 | 由内存大小决定 |
| UDP  | 65507 bytes | IPv4 标准限制，建议 ≤ 1472 |
| SERIAL| 无硬限制 | 由波特率 × 超时决定，建议 ≤ 4096 |
| FILE | 无硬限制 | 由磁盘空间决定 |

---

## 5. 按传输层的对接要求

### 5.1 SHM（共享内存）

| 项 | 要求 |
|----|------|
| OS 资源 | `/dev/shm/<name>`（POSIX 共享内存对象） |
| 库依赖 | 必须有 `shm_ipc` 库（或自行实现其协议） |
| 创建端 | 调用 `shm_create("/name")` 或 `shm_create_broadcast()` |
| 接入端 | 调用 `shm_open("/name")` 或 `shm_readn()` |
| 命名 | 必须以 `/` 开头，例 `/my_app_channel` |
| 清理 | 接入端不负责清理。创建端退出时应 `shm_destroy("/name")` |

**不对接 shm_ipc 库时**，可以实现以下等价操作：

1. `shm_open()` + `ftruncate()` + `mmap()` → 创建共享内存
2. 自己处理内部环形缓冲区的读写同步（xlink 用的 shm_ipc 内部使用锁 + 信号量 + 环形缓冲区）

**推荐**：直接链接 shm_ipc 库，或使用方案 A。

### 5.2 PIPE（命名管道）

| 项 | 要求 |
|----|------|
| OS 资源 | `/tmp/xlink_xxx.pipe`（FIFO 文件） |
| 创建端 | `mkfifo(path, 0666)` + `open(path, O_RDWR)` |
| 接入端 | `open(path, O_RDWR)` |
| 帧格式 | **必须**实现 [§4.1](#41-帧格式仅限流式传输) 的 4 字节长度帧 |
| 注意 | 无论是发送还是接收，都用 `O_RDWR` 打开（避免 open 阻塞） |

**最小对接（Python）：**

```python
import os, struct

fifo = os.open("/tmp/my_channel", os.O_RDWR)

def send(data):
    hdr = struct.pack("!I", len(data))
    os.write(fifo, hdr + data)

def recv():
    hdr = os.read(fifo, 4)
    n = struct.unpack("!I", hdr)[0]
    return os.read(fifo, n)
```

### 5.3 TCP（套接字）

| 项 | 要求 |
|----|------|
| OS 资源 | TCP 端口号（需可用，建议 ≥ 1024） |
| 服务端 | `socket()` + `bind()` + `listen()` + `accept()` |
| 客户端 | `socket()` + `connect()` |
| 帧格式 | **必须**实现 [§4.1](#41-帧格式仅限流式传输) 的 4 字节长度帧 |
| 地址举例 | 服务端：`:8080`；客户端：`192.168.1.5:8080` |

**最小对接（Node.js）：**

```javascript
const net = require('net');

const client = net.connect(8080, '192.168.1.5');
let buf = Buffer.alloc(0);

client.on('data', chunk => {
    buf = Buffer.concat([buf, chunk]);
    while (buf.length >= 4) {
        const len = buf.readUInt32BE(0);
        if (buf.length < 4 + len) break;
        const msg = buf.slice(4, 4 + len);
        buf = buf.slice(4 + len);
        // process msg
    }
});

function send(data) {
    const hdr = Buffer.alloc(4);
    hdr.writeUInt32BE(data.length);
    client.write(Buffer.concat([hdr, data]));
}
```

### 5.4 UDP（数据报）

| 项 | 要求 |
|----|------|
| OS 资源 | UDP 端口号 |
| 接收端 | `socket()` + `bind()` |
| 发送端 | `socket()` + `sendto()`（无需 bind） |
| 帧格式 | **不需要**帧格式。每个 `recvfrom()` 就是一条完整消息 |
| 地址举例 | 接收端：`:5555`；发送端：`224.0.0.1:5555`（组播） |

**最小对接（Go）：**

```go
import "net"

// 接收端
addr, _ := net.ResolveUDPAddr("udp", ":5555")
conn, _ := net.ListenUDP("udp", addr)
buf := make([]byte, 65536)
n, _, _ := conn.ReadFromUDP(buf)
// buf[:n] 是一条完整消息

// 发送端
raddr, _ := net.ResolveUDPAddr("udp", "224.0.0.1:5555")
conn, _ := net.DialUDP("udp", nil, raddr)
conn.Write([]byte("hello"))
```

### 5.5 SERIAL（串口）

| 项 | 要求 |
|----|------|
| 硬件 | UART 串口设备（物理 / USB 转串口 / PTY） |
| 设备节点 | `/dev/ttyUSB0`, `/dev/ttyS1`, `/dev/pts/X` |
| 帧格式 | **必须**实现 [§4.1](#41-帧格式仅限流式传输) 的 4 字节长度帧 |
| 波特率 | 默认 115200，可通过地址后缀配置（`/dev/ttyUSB0:9600`） |
| 数据位 | 8（固定） |
| 校验位 | 无（固定） |
| 停止位 | 1（固定） |

串口是**最弱**的传输——没有自动重传、没有流控（xlink 不启用硬件流控）。对接时注意：

- 解码端必须在无串口流控的场景下做消息边界恢复（4 字节头帧是最基本的方法）
- 缓冲区要有余量：`read()` 可能返回比请求更少的字节，需要用循环读取
- 高波特率下（≥115200）丢帧风险低；低波特率（9600）要考虑消息分片重组

**嵌入式对接（C，无 xlink 库）：**

```c
int serial_fd = open("/dev/ttyS1", O_RDWR | O_NOCTTY);
struct termios tio;
tcgetattr(serial_fd, &tio);
cfsetospeed(&tio, B115200);
cfsetispeed(&tio, B115200);
tio.c_cflag = CS8 | CREAD | CLOCAL;
tio.c_iflag = IGNPAR;
tio.c_cc[VMIN] = 1;
tio.c_cc[VTIME] = 0;
tcsetattr(serial_fd, TCSANOW, &tio);

// 发送：帧格式
uint32_t netlen = htonl((uint32_t)len);
write(serial_fd, &netlen, 4);
write(serial_fd, data, len);

// 接收：帧格式（循环读）
uint32_t netlen;
read_exact(serial_fd, &netlen, 4);
uint32_t plen = ntohl(netlen);
uint8_t* buf = malloc(plen);
read_exact(serial_fd, buf, plen);
```

### 5.6 FILE（文件 / 录制回放）

| 项 | 要求 |
|----|------|
| OS 资源 | 普通文件 |
| 录制模式 | `xlink_open(FILE)` 以 `XLINK_CREATE` → `writev` 序列化帧 |
| 回放模式 | `xlink_open(FILE)` 无 CREATE → `readv` 反序列化 |
| 帧格式 | 文件中所有消息串行化为 `[4 字节长度][载荷]` 序列 |
| 使用场景 | 离线录制 + 回放，非实时通信 |

文件不是一种"实时传输"，但如果你要保存/恢复通信历史，文件格式和 [§4.1](#41-帧格式仅限流式传输) 完全一致。

---

## 6. 地址约定（Rendezvous）

两个模块要通信，必须先约定好传输类型和地址。

### 6.1 推荐命名规范

```
SHM:   /xlink/<app>/<channel>     例 /xlink/sensor/data
PIPE:  /tmp/xlink/<app>/<channel> 例 /tmp/xlink/sensor/data
TCP:   <host>:<port>              例 192.168.1.100:9000 或 :9000（服务端）
UDP:   <host>:<port>              例 224.0.1.1:5000 或 :5000（接收端）
SERIAL: /dev/tty<path>:<baud>     例 /dev/ttyUSB0:115200
FILE:  <任意路径>                 例 /tmp/xlink_recording.bin
```

### 6.2 配置文件（建议）

模块对外暴露时，建议用一个配置文件声明连接信息：

```ini
# xlink_peer.ini
transport = tcp
address   = 192.168.1.100:9000
```

或环境变量：

```sh
export XLINK_PEER="tcp:192.168.1.100:9000"
```

---

## 7. 绑定 / 多语言支持

| 语言 | 推荐接入方式 |
|------|------------|
| **C / C++** | 直接链接 `libxlink.a`（方案 A） |
| **Python** | 实现 Wire Protocol over TCP 或 SHM（ctypes / mmap 方式） |
| **Go** | 实现 Wire Protocol over TCP 或 SHM |
| **Rust** | 实现 Wire Protocol over TCP 或 SHM（或 FFI 到 libxlink） |
| **Node.js** | `net` 模块 + TCP（见 [§5.3](#53-tcpsocket) 示例） |
| **Shell** | 通过命名管道（pipe）通信（见下方示例） |
| **嵌入式 C** | 实现 Wire Protocol over Serial（见 [§5.5](#55-serial串口)） |

### Shell / 脚本接入（通过命名管道）

```bash
# 模块 A（创建管道并监听）
mkfifo /tmp/xlink_to_b
exec 3<>/tmp/xlink_to_b          # O_RDWR 打开

# 封装 send/recv（需要 xxd 或 od 做二进制）
send_msg() {
    local len=${#1}
    printf "%.8x" $len | xxd -r -p  # 4 字节大端长度头
    printf "%s" "$1"
} >&3

recv_msg() {
    local hdr
    read -r -N4 hdr <&3                # 读 4 字节头
    local len=$((16#$(printf "%s" "$hdr" | xxd -p)))
    read -r -N$len msg <&3             # 读载荷
    printf "%s" "$msg"
}
```

---

## 8. 调试与验证

xlink CLI 工具源码位于 `tools/` 目录（`send.c` / `recv.c` / `bridge.c` / `monitor.c`），
但尚未加入 `Makefile` 构建系统。如需使用，可手动编译：

```sh
cd xlink
cc -I include -I third_party/shm_ipc/include tools/send.c bin/libxlink.a \
   -L third_party/shm_ipc/bin -lshm_ipc -lpthread -lrt -o bin/send
```

### 8.1 验证帧格式（无需 CLI 工具）

用 `xxd` 检查线缆格式是否正确：

```sh
# 监听管道
cat /tmp/xlink_test | xxd | head

# 输出示例（发送 "hi"）：
# 00000000: 0000 0002 6866                      ....hi
#                       ^^-- 4 字节长度头（2）
#                                ^^-- 载荷（"hi"）
```

### 8.2 验证测试

用现有测试套件验证功能完整性：

```sh
cd xlink && make clean && make test
```

---

## 9. 常见问题

### Q1: 我的模块只能收到一半消息

→ 可能是帧格式错误。检查是否用 `O_RDWR` 打开 pipe、是否在 TCP 上实现了 4 字节帧格式。

### Q2: 我的模块用 UDP，消息乱序或丢失

→ UDP 本来就是不可靠传输。如果需要有序可靠，改用 TCP 或 SHM。如果需要组播，UDP 是唯一选择。

### Q3: 两个模块同时 create SHM 会怎样？

→ 后创建的那个会失败。约定一个模块做创建端（XLINK_CREATE），其他做接收端（不传 CREATE 标志）。

### Q4: 模块在 Docker 容器里能用 SHM 吗？

→ 可以，但容器需要挂载 `/dev/shm`，且 `/dev/shm` 大小可能受限（默认 64MB）。推荐 TCP 作为跨容器传输。

### Q5: 为什么 pipe 的 open 不会阻塞？

→ xlink 固定用 `O_RDWR` 打开 FIFO。如果直接用 `open(O_RDONLY)` 或 `open(O_WRONLY)`，会阻塞等待另一端。

### Q6: 模块可以不处理帧格式吗？

→ 可以——用 `xlink_write()` / `xlink_read()` 跳过帧格式层，直接读写原始字节流。但对端也必须用同样的模式，否则消息边界会错乱。

### Q7: 模块崩溃了，SHM 段会残留吗？

→ xlink 有 `atexit` 清理。但如果进程被 `SIGKILL` 杀死（`kill -9`），`atexit` 不会执行，SHM 段会残留。重启后可以用 `monitor` 工具手动清理或使用 `ipcrm -M` 命令。

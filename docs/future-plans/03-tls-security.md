# TLS 加密通信层

> 优先级：P2 → **P1**（v2.1 目标）
> 依赖：异步 I/O 引擎（用于异步 TLS 握手） | 被依赖：无
> 预计工作量：约 3 周
> 最后更新：2026-06-25 （v1 实现完成）

## 动机

xlink 当前的 TCP 通信是明文的。对于跨网络部署（云端 ↔ 设备端），明文传输意味着：

- 数据可以被中间人嗅探
- 数据可以被篡改
- 无法验证对端身份

引入 TLS 可以让 xlink 在不依赖外部 VPN 或隧道的情况下获得端到端加密。

## 设计方案

### 接口设计

```c
// TLS 配置
typedef struct xlink_tls_config {
    const char *cert_file;      // 证书文件路径（PEM）
    const char *key_file;       // 私钥文件路径（PEM）
    const char *ca_file;        // CA 证书（用于验证对端）
    bool verify_peer;           // 是否验证对端证书
    const char *sni_hostname;   // SNI 主机名（客户端模式）
} xlink_tls_config_t;

// 在现有 API 上扩展
xlink_channel_t *xlink_open_tls(xlink_protocol_t proto,
                                const char *path, int flags,
                                xlink_tls_config_t *tls);
// flags 扩展：
//   XLINK_TLS          — 启用 TLS（等价于现有 xlink_open 加 tls 配置）
//   XLINK_TLS_SERVER   — 服务端模式（需要 cert + key）
//   XLINK_TLS_CLIENT   — 客户端模式（可选验证服务器）
```

### 架构

```text
用户代码
    │
    ▼
xlink API（xlink_send / xlink_read / xlink_wait）
    │
    ▼
TLS 封装层
    ├── SSL_read / SSL_write  （OpenSSL/BoringSSL/WolfSSL 适配）
    ├── SSL_accept / SSL_connect（TLS 握手）
    │
    ▼
TCP backend（socket I/O）
```

### 后端集成

TLS 可以作为 TCP backend 的一个可选包装层：

```c
// tcp_backend.c 内部
typedef struct tcp_peer {
    int fd;
    SSL *ssl;              // NULL = 明文模式
    xlink_tls_config_t tls_cfg;
    bool tls_enabled;
} tcp_peer_t;
```

`write_framed()` 和 `read_framed()` 在 `ssl != NULL` 时使用 `SSL_write`/`SSL_read` 而非 `write`/`read`。

## 实现路径

### Phase 1: 最小可行

- [ ] 定义 `xlink_tls_config_t` 和 `xlink_open_tls()` API
- [ ] OpenSSL 适配层（SSL_CTX 创建/销毁、握手、读写）
- [ ] TCP backend 中集成 TLS（包装 write_framed/read_framed）
- [ ] 测试：TLS echo server ↔ TLS echo client
- [ ] 测试：明文客户端 ↔ TLS 服务器拒绝连接

### Phase 2: 完善

- [x] **Per-client TLS 状态（2026-07-09）**：server 模式下每个客户端独立 SSL 对象
  - `tcp_priv_t` 新增 `client_tls[MAX_CLIENTS]` 数组，与 `client_fds` 并行维护
  - `tls.c` 新增 `tls_clone_for_client()` — 从共享 SSL_CTX 克隆独立 SSL
  - `tls.c` 新增 `xlink_tls_free_client_ssl()` — 释放 per-client SSL（不释放共享 CTX）
  - `recv_multi()` 重写 TLS 路径：查找对应 `client_tls[]`，临时 swap `ch->tls`
  - `send_to_all()` 同样支持 per-client TLS swap
  - `add_client`/`remove_client`/`tcp_backend_close` 都正确管理 per-client TLS 生命周期
- [ ] 证书验证回调（自定义验证逻辑）
- [ ] session 缓存 / session ticket
- [ ] ALPN 协商（应用层协议声明）
- [ ] OCSP stapling（证书吊销检查）
- [ ] 非阻塞握手（与异步 I/O 集成）
- [ ] 错误处理和详细错误信息

### Phase 3: 优化

- [ ] WolfSSL 支持（嵌入式场景，更小的二进制体积）
- [ ] BoringSSL 支持
- [ ] 硬件加速（如果平台支持）
- [ ] TLS 1.3 early data（0-RTT）
- [ ] 连接恢复（快速重连）

## Phase 2 深化设计（2026-07-07）

### Per-Client TLS（服务器模式）

**问题**：当前 TLS 服务器模式下所有客户端共享同一个 `SSL *` 对象，导致：
- 客户端 A 握手成功后，客户端 B 的连接会覆盖 SSL 状态
- 无法对单个客户端做证书吊销或连接关闭
- 多客户端并发数据混入同一个 SSL 缓冲区

**方案**：TCP 后端维护 per-fd TLS 映射表：

```c
/* tcp_backend.c — per-client TLS state */
#define MAX_TLS_CLIENTS 64

typedef struct {
    int   fd;
    SSL  *ssl;          /* per-client SSL object */
    int   handshake_done;
    char  peer_cn[256]; /* X509 common name (optional) */
} tls_client_t;

typedef struct {
    /* Server mode per-client TLS */
    tls_client_t tls_clients[MAX_TLS_CLIENTS];
    int          n_tls_clients;

    /* Server's shared SSL_CTX (from xlink_tls_configure) */
    SSL_CTX     *server_ctx;
    int          is_tls_server;
} tcp_priv_t;
```

**accept 路径**：
1. `accept()` 返回新 fd
2. 在 tls_clients[] 中查找空闲 slot
3. `SSL_new(server_ctx)` → `SSL_set_fd(ssl, fd)` → `SSL_accept(ssl)`（非阻塞，见下方）
4. 握手完成后从 `X509` 提取 `peer_cn`

**send/recv 路径**：
- 查找 fd 对应的 `tls_client_t *`
- 使用其 `ssl` 做 SSL_write/SSL_read
- 如果该 slot 为 NULL → 走明文路径（兼容非 TLS 客户端）

**cleanup**：
- `shutdown_fd()` 时释放对应 `tls_client_t` slot
- 从 `client_fds[]` 移除后同时从 `tls_clients[]` 移除
- `SSL_shutdown()` + `SSL_free()`

### 非阻塞 TLS 握手（与异步 I/O 集成）

**目标**：TLS 握手不再阻塞整个线程，适配 `xlink_run()` 事件循环。

**关键点**：
- OpenSSL 在非阻塞 socket 上调用 `SSL_accept()` / `SSL_connect()` 可能返回 `SSL_ERROR_WANT_READ` 或 `SSL_ERROR_WANT_WRITE`
- 此时不能视为错误，需要等待 socket 可读/可写后重试
- 这与 `xlink_run()` 的事件驱动模型天然兼容

**实现方案**：

```c
/* aio.c — TLS handshake integration into event loop */

/* Per-channel handshake state */
typedef enum {
    HS_IDLE,          /* no TLS or already done */
    HS_WANT_READ,     /* need socket readable to continue */
    HS_WANT_WRITE,    /* need socket writable to continue */
    HS_DONE,
    HS_FAILED
} handshake_state_t;

/* Added to xlink_channel_t internal */
typedef struct xlink_channel {
    /* ... existing fields ... */
    handshake_state_t hs_state;   /* TLS handshake progress */
    /* ... */
} xlink_channel_t;
```

**xlink_run() 集成**：

```
xlink_run 主循环:
  for each channel with hs_state ∈ {HS_WANT_READ, HS_WANT_WRITE}:
    1. epoll 监听 fd（EPOLLIN 或 EPOLLOUT）
    2. 事件触发 → 调用 tls_do_handshake_nonblock(ch)
    3. 函数返回:
       - 0 (HS_DONE) → 转为正常 read/write 模式
       - SSL_ERROR_WANT_READ/WRITE → 更新 hs_state，下轮继续
       - 其他 → 触发 on_error 回调
```

**非阻塞握手函数**：

```c
static int tls_do_handshake_nonblock(xlink_channel_t *ch) {
    tls_state_t *ts = ch->tls;
    int ret = ts->is_server ? SSL_accept(ts->ssl)
                            : SSL_connect(ts->ssl);

    if (ret == 1) {
        ts->handshake_done = 1;
        ch->hs_state = HS_DONE;
        return 0;
    }

    int ssl_err = SSL_get_error(ts->ssl, ret);
    switch (ssl_err) {
    case SSL_ERROR_WANT_READ:
        ch->hs_state = HS_WANT_READ;
        return 1;  /* caller should wait for EPOLLIN */
    case SSL_ERROR_WANT_WRITE:
        ch->hs_state = HS_WANT_WRITE;
        return 1;  /* caller should wait for EPOLLOUT */
    default:
        ch->hs_state = HS_FAILED;
        /* record error in ch->errbuf */
        return -1;
    }
}
```

### ALPN 协商

**目标**：允许应用通过 TLS 协商协商应用层协议（例如区分 binary 帧协议 vs JSON）。

**API**：

```c
/* 新增到 xlink_tls_config_t */
typedef struct xlink_tls_config {
    /* ... existing fields ... */
    const char *alpn_protos;     /* comma-separated, e.g. "xlink/1,xlink/json" */
    char        alpn_negotiated[64]; /* output: negotiated protocol */
} xlink_tls_config_t;
```

**OpenSSL 实现**：
```c
static int tls_setup_alpn(SSL_CTX *ctx, const char *protos) {
    /* "xlink/1,xlink/json" → 8\xlink/1\x0a\xlink/json */
    /* 或使用 SSL_set_alpn_protos() 带长度前缀格式 */
    unsigned char wire[256];
    int len = alpn_encode(protos, wire, sizeof(wire));
    if (len < 0) return -1;
    SSL_CTX_set_alpn_protos(ctx, wire, len);
    return 0;
}

/* 握手完成后读取协商结果 */
static void tls_get_alpn_result(SSL *ssl, xlink_tls_config_t *cfg) {
    const unsigned char *data;
    unsigned int len;
    SSL_get0_alpn_selected(ssl, &data, &len);
    if (data && len < sizeof(cfg->alpn_negotiated)) {
        memcpy(cfg->alpn_negotiated, data, len);
        cfg->alpn_negotiated[len] = '\0';
    }
}
```

### 会话恢复（TLS Session Cache）

**目标**：TLS 重连时跳过完整握手，减少 1-RTT 延迟。

**方案**（Phase 2 scope — 仅客户端）：
```c
typedef struct xlink_tls_config {
    /* ... existing fields ... */
    const char *session_file;    /* 持久化 session ticket 到文件 */
    int         session_timeout; /* 默认 300 秒 */
} xlink_tls_config_t;
```

- `SSL_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT)`
- 成功握手后 `SSL_SESSION *s = SSL_get1_session(ssl)` → 序列化到 session_file
- 下次连接：`SSL_set_session(ssl, session)` → 尝试会话恢复
- 服务器无需改动（session ticket 由 client 存储和发送）

### Phase 2 实现步骤

| 步骤 | 内容 | 预计工作量 |
|------|------|-----------|
| 2.1 | Per-client TLS 映射表 + multi-client accept 路径 | ~2天 |
| 2.2 | 非阻塞握手 + xlink_run 集成 | ~3天 |
| 2.3 | ALPN 协商 | ~1天 |
| 2.4 | TLS 会话缓存（客户端） | ~1天 |
| 2.5 | 补充测试：per-client + nonblock + ALPN | ~2天 |
| 2.6 | 更新文档 | ~0.5天 |

## 依赖

- **Phase 1-2**: OpenSSL >= 1.1.1（或 LibreSSL）
- **Phase 3**: WolfSSL（嵌入式）/ BoringSSL（Android）
- **前置依赖**: 异步 I/O（[02-async-io.md](02-async-io.md)）—— 非阻塞握手需要事件循环
- **前置依赖**: xlink_run() 事件回调 — 已在 v2.1 实现 ✅

## 开放问题

1. **API 复杂度**: `xlink_open_tls()` 是否需要单独的 API？还是通过 flag 扩展 `xlink_open()`？（`XLINK_TLS` flag 方式更简洁）
2. **证书管理**: 嵌入证书 vs 文件路径 vs 内存回调？嵌入式场景可能没有文件系统。
3. **性能影响**: TLS 加密/解密对吞吐量的影响。是否需要区分控制通道和数据通道的加密策略？
4. **最小二进制**: 对于嵌入式平台，OpenSSL 体积较大（~2MB）。WolfSSL 可以缩减到 ~100KB。

## 关联文档

- [插件化架构](01-plugins-arch.md) — TLS 可以作为可选插件加载
- [异步 I/O 支持](02-async-io.md) — 非阻塞 TLS 握手需要事件驱动
- [跨平台支持](05-multi-platform.md) — 不同平台 TLS 库适配

## 实现状态（2026-06-25）

**v1 已完成**（commit `746ef72`）：

### 已实现
- ✅ `XLINK_TLS` flag, `xlink_tls_config_t`, `xlink_tls_configure()` / `xlink_tls_enabled()`
- ✅ `src/tls.c` — OpenSSL 适配层（SSL_CTX 管理、握手、读写、清理）
- ✅ TCP 客户端/服务器模式 TLS 集成（via `write_framed_tls` / `read_framed_tls`）
- ✅ 服务器多客户端 recv_multi TLS 路径
- ✅ 大负载精确读取（loop `SSL_read` 保证读满请求量）
- ✅ Makefile 双构建路径（`make all` 不引入 TLS 依赖，`make tls` 启用 OpenSSL）
- ✅ 5 个测试用例，0 failures

### 设计决策
- **API 简化**：未引入 `xlink_open_tls()` 独立函数；使用 `xlink_open()` + `xlink_tls_configure()` 两步模式，保持向后兼容
- **延迟握手**：握手延迟到首次 I/O（`tls_do_handshake` 在 read/write 时触发）
- **仅 TCP**：TLS 仅支持 TCP 后端；`xlink_tls_configure()` 对非 TCP 通道返回 -1
- **Blocking I/O**：当前使用阻塞式 `SSL_read`/`SSL_write`（v1 简化）

### 已知限制
- 服务器多客户端 TLS 复用同一个 SSL 对象（Per-client TLS 待实现）
- 无异步 TLS 握手（blocking socket）
- 无证书吊销检查（CRL/OCSP）

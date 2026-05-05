# TLS 加密通信层

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

## 依赖

- **Phase 1-2**: OpenSSL >= 1.1.1（或 LibreSSL）
- **Phase 3**: WolfSSL（嵌入式）/ BoringSSL（Android）
- **前置依赖**: 异步 I/O（[02-async-io.md](02-async-io.md)）—— 非阻塞握手需要事件循环

## 开放问题

1. **API 复杂度**: `xlink_open_tls()` 是否需要单独的 API？还是通过 flag 扩展 `xlink_open()`？（`XLINK_TLS` flag 方式更简洁）
2. **证书管理**: 嵌入证书 vs 文件路径 vs 内存回调？嵌入式场景可能没有文件系统。
3. **性能影响**: TLS 加密/解密对吞吐量的影响。是否需要区分控制通道和数据通道的加密策略？
4. **最小二进制**: 对于嵌入式平台，OpenSSL 体积较大（~2MB）。WolfSSL 可以缩减到 ~100KB。

## 关联文档

- [插件化架构](01-plugins-arch.md) — TLS 可以作为可选插件加载
- [异步 I/O 支持](02-async-io.md) — 非阻塞 TLS 握手需要事件驱动
- [跨平台支持](05-multi-platform.md) — 不同平台 TLS 库适配

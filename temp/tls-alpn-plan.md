# TLS ALPN 协商 + IPC 后端 — 完成记录
创建: 2026-07-30 | 完成: 2026-08-02

## TLS ALPN 协商
- [x] 1: 扩展 xlink_tls_config_t（新增 alpn_protos、alpn_negotiated）
- [x] 2: tls_create_ctx() 设置 SSL_CTX_set_alpn_protos()（服务器端 select callback）
- [x] 3: 客户端 SSL_set_alpn_protos() 设置通告协议
- [x] 4: 握手完成后读取协商结果到 alpn_negotiated + xlink_tls_alpn_negotiated() API
- [x] 5: test_tls_alpn.c 4 用例（matching / no-match / client-only / server-only）
- [x] 6: make tls 验证通过
- [x] 7: 修复 no-match 场景测试挂起（RFC 7301 强制拒绝语义 + server 看门狗）

## IPC (AF_UNIX) 后端
- [x] src/ipc_backend.c — AF_UNIX SOCK_STREAM，client/server 模式，地址格式 /path、.sock:、ipc://
- [x] tests/test_ipc.c — 39 checks 全过
- [x] Makefile + plugin.c + xlink_internal.h + xlink.c 接线
- [x] test_plugin.c 断言 6→7（新增内置后端）

## 验证
- make all 0 warnings（新增代码无警告）
- make test 全部套件 PASS（含新增 test_ipc 39/39）
- make tls_tests test_tls_alpn 4/4 PASS

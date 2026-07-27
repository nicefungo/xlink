# TCP MSG_ZEROCOPY 实现计划

创建时间: 2026-07-27 08:05

## 目标
在 tcp_backend.c 中实现 tcp_send_zc()，使用 Linux sendmsg(MSG_ZEROCOPY) 实现零拷贝发送。

## 步骤

- [x] Step 2.4a: 实现 tcp_send_zc_impl() — sendmsg + MSG_ZEROCOPY
- [x] Step 2.4b: 实现 tcp_drain_zc_completions — 从 socket error queue 读取 SO_EE_ORIGIN_ZEROCOPY 事件
- [x] Step 2.4c: 注册 vtable 方法（send_zc, zc_capable）
- [x] Step 2.4d: 测试: test_zc_tcp.c（25 checks, 0 failures）
- [x] Step 2.4e: 集成 xlink_zc_poll() — 自动 drain TCP error queue

## 当前进度
全部完成。25 个测试全部通过。

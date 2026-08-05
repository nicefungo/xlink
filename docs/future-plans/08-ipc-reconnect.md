# IPC 错误恢复与自动重连 (AF_UNIX)

## 动机

AF_UNIX (`src/ipc_backend.c`) 作为 xlink 最快的传输后端（比 localhost TCP 快 ~30%），
其客户端在服务端重启 / 崩溃 / 主动关闭时必然断线。若客户端进程长期运行（守护进程、
agent、日志转发器），**断线后必须自动重连**，否则链路静默失效。

2026-08-04 已实现 v1（`try_reconnect` 指数退避 + SIGPIPE 安全），本计划文档记录现状、
评估缺口，并规划 v2 增强。设计目标：**断线自愈、零数据静默丢失、可观测**。

## 设计方案

### v1（已实现，2026-08-04 提交 `d187820`）

| 能力 | 实现 |
|------|------|
| 自动重连 | `try_reconnect()`：断线后按 100→200→400…→5000ms 指数退避重连（镜像 TCP 后端） |
| SIGPIPE 安全 | `ipc_sendv()`：`sendmsg(MSG_NOSIGNAL)` 覆盖 3 处写路径，断线以 EPIPE/ECONNRESET 上报而非杀进程 |
| server 清理 | `ipc_close(server)` unlink socket 文件，不留悬空 `.sock` |
| 测试 | `test_auto_reconnect`：server 重启后客户端自动重连，test_ipc 48/48 PASS |

关键点：重连不重建 `xlink_channel_t`，复用 channel 内部状态；recon_backoff 归零表示已连接。

### v2（规划）

#### 2.1 重连状态钩子（可观测性）

断线/重连目前对用户透明。长时间断线（server 宕机）用户无法感知链路已降级。

```c
/* 可选的连接状态回调 */
typedef void (*xlink_ipc_state_fn)(xlink_channel_t *ch,
                                   int state,      /* XLINK_IPC_CONN_UP/DOWN/RECONNECTING */
                                   const char *reason, void *ud);

int xlink_ipc_set_state_cb(xlink_channel_t *ch,
                           xlink_ipc_state_fn cb, void *ud);
```

状态机：`CONNECTED → (err) → RECONNECTING{(backoff)} → CONNECTED | FAILED(timeout)`。
用户可在 DOWN 时暂停发送，UP 时恢复。

#### 2.2 退避抖动（jitter）与失败上限

当前固定指数退避，多客户端同时挂掉时会同频重连（惊群式 thundering herd）。
改进：
- 每次退避叠加随机抖动 ±20%（`backoff + rand()% (backoff/5)`），错开重连时刻。
- 增加 `max_retries` 或 `total_timeout`（默认 60s）→ 超过后回调 FAILED，用户可决定放弃或重置。

#### 2.3 断线期间发送缓冲（可选）

断线瞬间用户调用 `xlink_send()` 返回 -1。v2 可提供 `xlink_ipc_buffered_mode(ch, on)`：
断线期间消息写入内部环形缓冲（复用 `spsc_queue`），重连成功后按序 flush。
需权衡：缓冲上限（默认 1MB）、丢弃策略（head-drop，保最新）、延迟语义（重连后旧消息突发）。

#### 2.4 空闲探测（keepalive）

AF_UNIX 断连通常由 peer close 立即触发（EPIPE），无需心跳。但仍需覆盖：
- 对端半关闭（只关写端）
- 网络命名空间切换等罕见场景
可选：`XLINK_IPC_KEEPALIVE` flag 启用周期 `send(fd, NULL, 0, MSG_NOSIGNAL)` 探测。

## 实现路径

- **Phase 1 (v1, ✅ 2026-08-04)**: 指数退避重连 + SIGPIPE 安全 + server unlink + 测试
- **Phase 2**: 连接状态回调 `xlink_ipc_set_state_cb` + 状态机 + 测试（预期 ~0.5 天）
- **Phase 3**: 退避抖动 + max_retries 失败上限（预期 ~0.5 天）
- **Phase 4**: 断线发送缓冲（复用 spsc_queue，预期 ~1 天）
- **Phase 5**: 空闲探测 keepalive（预期 ~0.5 天）

## 依赖

- v2.1 异步 I/O 已交付（重连可在 `xlink_run()` 事件循环中驱动，替代线程内 usleep 轮询）
- `src/spsc_queue.h`（Phase 4 缓冲复用）
- TCP 后端的重连逻辑为模板（`try_reconnect` 镜像自 TCP）

## 开放问题

- 重连应由专用线程驱动还是接入 `xlink_run()` 事件循环？（前者简单但阻塞退出，后者与 AIO 统一但需处理重连 fd 的 epoll 注册）
- 断线缓冲的延迟语义是否可接受？高实时场景应默认关闭。

## 关联文档

- [index.md](index.md) 路线图总览
- `docs/design-decisions.md` 决策 5a（IPC 重连 + SIGPIPE 安全）
- `src/ipc_backend.c` 实现
- `tests/test_ipc.c` / `tests/test_auto_reconnect`
- [05-multi-platform.md](05-multi-platform.md) 跨平台（IPC ↔ Windows 命名管道映射）

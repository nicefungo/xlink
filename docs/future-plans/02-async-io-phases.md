# xlink v2.0 Phase 2: 异步 I/O 设计文档

> 创建: 2026-05-29 | 依赖: Phase 1 插件化 ✅
> v2.0 已发布 (2026-06-01) — 步骤 2.1-2.4 交付。步骤 2.5-2.9 → v2.1

---

## 1. 目标

将 xlink 的 `xlink_wait()` 从轮询模式升级为事件驱动模式，用 `epoll`/`io_uring` 替代 `poll() + usleep()`。

核心改动范围：
- 新增 3 个内部源文件：`aio.h`, `aio_epoll.c`, `aio_poll.c`
- 新增公共 API：`xlink_aio_create/destroy`, `xlink_wait_aio()`
- 零破坏现有 API，`xlink_wait()` 完全保留

---

## 2. 架构设计

### 2.1 引擎层次

```
应用程序
    │
    │ xlink_wait_aio(chans, n, timeout, aio)
    ▼
┌────────────────────────────────────────┐
│            aio.c  (引擎注册表)           │
│  - xlink_aio_create(type) → 引擎实例    │
│  - xlink_wait_aio() → 事件驱动等待       │
└────────────────────────────────────────┘
    │
    ├── aio_epoll.c  (Linux epoll)
    │       .watch  = epoll_watch
    │       .wait   = epoll_wait_fn
    │
    └── aio_poll.c   (POSIX poll 回退)
            .watch  = poll_watch
            .wait   = poll_wait_fn
```

### 2.2 引擎选择策略

```
xlink_aio_create(AUTO):
  → 尝试 epoll_create1()  → 成功 → epoll 引擎
  → 失败                    → poll 引擎
```

### 2.3 公共 API (xlink.h)

```c
// 创建引擎（0=AUTO, 1=POLL, 2=EPOLL, 3=IO_URING）
void *xlink_aio_create(int type);
void  xlink_aio_destroy(void *engine);
const char *xlink_aio_name(void *engine);

// 事件驱动版 wait（API 层面替代 xlink_wait）
int xlink_wait_aio(xlink_channel_t **chans, int n,
                   int timeout_ms, void *aio_engine);
```

---

## 3. 实现步骤

### 步骤 2.1: aio.h 引擎抽象 ✅
- 定义 `xlink_aio_t`, `xlink_aio_ops` (vtable)
- 内部使用，不暴露给用户

### 步骤 2.2: aio_epoll.c ✅
- `epoll_create1(EPOLL_CLOEXEC)` 创建 epoll 实例
- `.watch()` 注册 channel fd → EPOLLIN
- `.wait()` 调用 `epoll_wait()` 返回就绪 channel

### 步骤 2.3: aio_poll.c ✅
- POSIX poll 封装，与现有 `poll()` 行为一致

### 步骤 2.4: aio.c 引擎管理 ✅
- `xlink_aio_create()` 创建引擎实例
- `xlink_wait_aio()` → 注册 channels → wait → peek SHM → 返回

### 步骤 2.5: SHM eventfd 唤醒
- SHM 写入端写 eventfd 通知
- `xlink_wait_aio()` epoll 监听 eventfd + peek
- 不再需要 `usleep()` 轮询

### 步骤 2.6: xlink_run() 事件回调
- `void xlink_run(chans, n, aio, callback, arg)`
- 事件驱动主循环

### 步骤 2.7: io_uring 引擎
- `IORING_OP_READ/WRITE/ACCEPT` → 提交 SQE → wait CQE
- 基准对比 epoll vs io_uring

### 步骤 2.8: 测试 + 基准
- `test_aio.c` — 验证 create/destroy/wait
- `test_aio_perf.c` — 基准测试

### 步骤 2.9: 更新文档

---

## 4. 当前进度

- [x] 2.1-2.4: 基础引擎实现 + 构建验证 — **v2.0 已交付**
- [x] 2.8: `test_aio.c` — create/destroy/wait timeout/pipe/SHM/mixed (26 checks)
- [x] 2.5: SHM eventfd → **v2.1** ✅ (shm_backend.c FIFO + aio.c drain/verify, test_aio.c 35/35 PASS)
- [ ] 2.6: xlink_run() → **v2.1**
- [x] 2.6: xlink_run() → **v2.1** ✅ (aio.c event loop + peek stale detection, test_run.c 24/24 PASS)
- [ ] 2.7: io_uring → **v2.1**
- [x] 2.7: io_uring → **v2.1** ✅ (aio_uring.c raw syscall impl, aio.c registry, 0 warnings, all tests pass)
- [ ] 2.9: 文档 → **v2.1**

## 5. 依赖

| 项 | 状态 |
|----|------|
| Phase 1 插件化 | ✅ |
| Linux 2.6+ (epoll) | ✅ |
| Linux 5.1+ (io_uring) | 可选 |
| SHM eventfd | 可选 |

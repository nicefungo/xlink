# Step 2.7: io_uring 引擎实现计划
创建时间: 2026-06-16 14:45

## 目标
新增 src/aio_uring.c，实现 linux io_uring 引擎，对接 aio.h 的 vtable。

## 步骤
- [ ] 1. 创建 src/aio_uring.c — io_uring 引擎实现
- [ ] 2. 更新 aio.c — 注册 io_uring 到 create_impl
- [ ] 3. 更新 Makefile — 添加 aio_uring.o
- [ ] 4. make clean && make all — 验证编译通过
- [ ] 5. make test — 全量测试通过
- [ ] 6. 更新 phases.md — 标记步骤 2.7 完成
- [ ] 7. git commit

## 设计要点
- 使用 liburing（或 fallback: 直接 syscall 使用 io_uring_setup/enter/register）
- ops: init/fini/watch/unwatch/wait
- watch → IORING_OP_POLL_ADD
- unwatch → IORING_OP_POLL_REMOVE
- wait → submit + wait CQE
- 如果没有 liburing，使用 raw syscall 接口
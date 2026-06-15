# xlink v2.1 步骤 2.5 — SHM eventfd 唤醒 执行计划

创建时间: 2026-06-09 14:05

## 目标
SHM channel 写入端使用 eventfd 通知 epoll,不再依赖 peek 轮询

## 步骤
- [ ] 1. xlink_internal.h: xlink_channel_t 添加 event_fd 字段
- [ ] 2. shm_backend.c: shm_backend_open() 创建 eventfd
- [ ] 3. shm_backend.c: shm_backend_send() 后 write(eventfd, &val, sizeof(val))
- [ ] 4. shm_backend.c: shm_backend_close() 时 close(eventfd)
- [ ] 5. aio.c: xlink_wait_aio_impl() 将 SHM channel 的 eventfd 注册到 epoll
- [ ] 6. aio.c: epoll 返回后, read(eventfd) 消耗事件, 然后 peek 确认数据
- [ ] 7. tests/test_aio.c: 添加 SHM eventfd 测试用例
- [ ] 8. make clean && make all && make test 验证
- [ ] 9. git commit + 更新 phases.md

## 当前进度
正在执行: 步骤 1
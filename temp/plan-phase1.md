# xlink Phase 1 — 项目骨架搭建

创建时间: 2026-04-27

## 目标

建立 xlink 项目的完整目录结构，核心 API 头文件，中央调度层，以及 SHM/Pipe 两个后端的完整实现。

## 步骤

- [x] 创建项目目录树
- [x] 写 README.md — 项目概览
- [x] 写 Makefile — 模块化构建（backends / tools / examples / tests）
- [ ] 写 include/xlink.h — 公共 API（open/send/recv/close + 类型 + 选项）
- [ ] 写 src/xlink.c — 中央调度层 + framing 层
- [ ] 写 src/shm_backend.c — 封装 shm_ipc
- [ ] 写 src/pipe_backend.c — 命名管道
- [ ] 写 tools/send.c — xlink-send CLI
- [ ] 写 tools/recv.c — xlink-recv CLI
- [ ] 写 docs/api.md — API 参考
- [ ] 移动 proposal 到 docs/
- [ ] 构建验证：make clean && make all
- [ ] 运行验证：双向 SHM 通信测试

## 当前进度

正在执行: 创建目录树

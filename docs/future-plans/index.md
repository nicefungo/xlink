# xlink 未来规划 — 路线图总览

> 最后更新：2026-05-06

## 状态总览

| 领域 | 状态 |
|------|------|
| P0 短期改进（3项） | ✅ **全部实现并提交**（Round 44） |
| 已知问题 #2 (SHM 64 cap) | ✅ **已修复**（→256） |
| 已知问题 #7 (Makefile wildcard) | ✅ **已修复**（自动发现） |
| 已知问题 #8 (UDP EAGAIN errbuf) | ✅ **已修复**（清晰消息） |
| 剩余已知问题 | 5 项（#1, #3, #4, #5, #6） |

## 优先级定义

| 级别 | 说明 |
|------|------|
| **P0** | 下一版本必须包含（影响正确性或可用性） |
| **P1** | 重要但可推迟到后续版本 |
| **P2** | 有价值但无明确时间表 |

## 路线图

### ✅ 已实现（2026-05）

| 计划 | 版本 | 提交 |
|------|------|------|
| TCP 错误路径 xlink_errstr 补充 | ✅ Done | `c9a657b` / `d635e09` |
| read_exact 内部超时保护（30s） | ✅ Done | `978f8d0` |
| NONBLOCK TCP send EAGAIN 重试 | ✅ Done | `dcd43f0` |
| Makefile wildcard 测试自动发现 | ✅ Done | `99f560d` |
| UDP NONBLOCK 清晰 errbuf | ✅ Done | `4e04816` |
| SHM atexit cleanup 扩至 256 | ✅ Done | `0d13a4d` |

### 近期（P0 — 下一版本）

暂无 P0 待实现项（所有已知短期改进已完成）。

### 中期（P1 — 后续版本）

| 计划 | 优先级 | 依赖 | 预计工作量 |
|------|--------|------|-----------|
| 异步 I/O 支持（io_uring） | P1 | 性能分析数据 | 大（~2周） |
| 插件化架构 | P1 | 无 | 大（~2周） |

### 远期（P2 — 未来版本）

| 计划 | 优先级 | 依赖 | 预计工作量 |
|------|--------|------|-----------|
| TLS 加密通信层 | P2 | 异步 I/O | 大（~3周） |
| 性能优化（zero-copy、批量化） | P2 | 异步 I/O | 中（~1周） |
| 跨平台支持 | P2 | 插件化架构 | 极大（~2月） |

## 依赖关系图

```text
    ——— 已实现 ———
TCP 错误信息补充 ────── 无依赖
read_exact 超时保护 ──── 无依赖
NONBLOCK EAGAIN 重试 ─── 无依赖
Makefile wildcard ────── 无依赖
UDP EAGAIN errbuf ────── 无依赖
SHM atexit 256 ───────── 无依赖
          │
    ——— 未来 ———
          │
          ▼
    插件化架构 ─────────┐
          │             │
          ▼             ▼
    异步 I/O ──────── TLS 加密
     (io_uring)         (依赖异步 I/O)
          │
          ├────────── 性能优化 (zero-copy)
          │
          └────────── 跨平台支持
                        (依赖插件化架构)
```

## 决策日志

| 日期 | 决策 | 背景 |
|------|------|------|
| 2026-05-06 | 所有短期 P0 项已实现 | TCP errbuf、read_exact 超时、EAGAIN 重试，全部 commit 并测试通过 |
| 2026-05-06 | 已知问题减少至 5 项 | #2 (SHM cap) #7 (Makefile) #8 (UDP errbuf) 已修复 |
| 2026-05-06 | design-decisions.md 发布 | 记录 8 项设计决策，防止被误判为 bug |
| 2026-05-05 | 确定文档优先策略 | 代码质量稳定（41轮零 bug），转向规划未来方向 |
| 2026-05-05 | 将代码审查降为每周一次 | 连续 41 轮审查无新 bug，cron job 维持日常覆盖 |

## 相关文档

- [P0 短期改进（已实现 → 归档）](archive/06-p0-short-term.md)
- [插件化架构](01-plugins-arch.md)
- [异步 I/O 支持](02-async-io.md)
- [TLS 加密通信层](03-tls-security.md)
- [性能优化](04-performance.md)
- [跨平台支持](05-multi-platform.md)
- [代码审查记录](/home/admin/xlink/docs/next-version-thoughts.md)
- [已知问题](/home/admin/xlink/docs/known-issues.md)

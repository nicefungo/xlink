# xlink 未来规划 — 路线图总览

> 最后更新：2026-05-11

## 状态总览

| 领域 | 状态 |
|------|------|
| P0 短期改进（3项） | ✅ **全部实现并提交**（Round 44） |
| 已知问题 #2 (SHM 64 cap) | ✅ **已修复**（→256） |
| 已知问题 #7 (Makefile wildcard) | ✅ **已修复**（自动发现） |
| 已知问题 #8 (UDP EAGAIN errbuf) | ✅ **已修复**（清晰消息） |
| 已知问题 #1 (.read timeout) | ✅ **大部分修复**（5/6 后端已实现 poll-based .read） |
| 剩余已知问题 | 5 项（#1 仅剩 SHM、#3、#4、#5、#6） |

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

### 近期已完成（2026-05-07）

| 计划 | 版本 | 提交 |
|------|------|------|
| TCP .read vtable (poll-based timeout) | ✅ Done | `abf9880` |
| Pipe/Serial/UDP .read vtable | ✅ Done | `ba66ba7` |
| File .read vtable (poll) | ✅ Done | `7ce9b15` |
| test_read_timeout 验证 | ✅ Done | `0331ba6` |
| code-walkthrough.md 架构文档 | ✅ Done | `279b8e0` |

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
| 2026-05-07 | .read vtable 实现（5个后端） | TCP、Pipe、Serial、UDP、File 后端均已实现 poll-based 超时 read。xlink_read(timeout_ms) 的已知问题 #1 从"所有后端"缩小到"仅 SHM"。含 test_read_timeout 验证（6 checks）。 |
| 2026-05-07 | code-walkthrough.md 发布 | 完整架构文档（27KB，442行），覆盖全部 6 个后端和代码数据流 |
| 2026-05-08 | 第 46 轮周期审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。docs 检查：integration-guide.md/proposal.md 仍准确，slab-allocator.md 保持草案状态。未来规划文档同步更新。 |
| 2026-05-09 | 第 47 轮周期审查 | 30 test binaries ALL PASS, 0 警告。docs 检查已通过，Round 46 未提交的文档修改已补提（4 文件）。未来规划仍准确，无新增 P0。 |
| 2026-05-05 | 确定文档优先策略 | 代码质量稳定（41轮零 bug），转向规划未来方向 |
| 2026-05-05 | 将代码审查降为每周一次 | 连续 41 轮审查无新 bug，cron job 维持日常覆盖 |
| 2026-05-10 | 第 48 轮周期审查 | 30 test binaries ALL PASS (230 checks), 0 警告, 0 bug。代码库无新提交。所有 docs 检查通过。future-plans/ 仍准确，无新增 P0。 |
| 2026-05-11 | 第 49 轮周期审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 49 轮连续干净审查。docs 检查：所有文档仍准确，代码库自 Round 48 起无新提交。已知问题保持 5 项不变。 |

## 相关文档

- [P0 短期改进（已实现 → 归档）](archive/06-p0-short-term.md)
- [插件化架构](01-plugins-arch.md)
- [异步 I/O 支持](02-async-io.md)
- [TLS 加密通信层](03-tls-security.md)
- [性能优化](04-performance.md)
- [跨平台支持](05-multi-platform.md)
- [代码审查记录](/home/admin/xlink/docs/next-version-thoughts.md)
- [已知问题](/home/admin/xlink/docs/known-issues.md)

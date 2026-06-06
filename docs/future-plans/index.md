# xlink 未来规划 — 路线图总览

> 最后更新：2026-06-04

## 状态总览

| 领域 | 状态 |
|------|------|
| P0 短期改进（3项） | ✅ **全部实现并提交**（Round 44） |
| 已知问题 #1 (SHM .read timeout) | ✅ **已修复**（2026-05-28） |
| v1.0 技术报告 | ✅ **已发布**（`docs/technical-report.md`） |
| **🆕 v2.0: 插件化 + 异步 I/O** | ✅ **已发布**（2026-06-01, tag `v2.0`） |
| 剩余已知问题 | 4 项 by-design/minor（#3/#4/#5/#6） |

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

### ✅ v2.0 — 已发布（2026-06-01）

**tag: `v2.0`** · 主题：插件化 + 异步 I/O

| 计划 | 状态 | 提交 |
|------|------|------|
| 插件化架构（Phase 1） | ✅ | `4c7faf7`, `0ffc5a5` |
| 异步 I/O 基础（Phase 2） | ✅ | `b0e3dac`, `63b0c3d` |

**v2.0 交付清单**：
- `xlink_plugin_load()` / `xlink_open_url()` — `.so` 动态加载，42 checks
- `xlink_wait_aio()` / `xlink_aio_create()` — epoll/poll 引擎，26 checks
- 新增 4 个源文件: `src/plugin.c`, `src/aio.c`, `src/aio_epoll.c`, `src/aio_poll.c`
- 32 test binaries, ALL PASS, 0 warnings
- 完全向后兼容，`xlink_wait()` 保持不变
- 文档：code-walkthrough.md §16-17，integration-guide.md §2.6

### 近期（P0 — 下一版本：v2.1）

**v2.1 主题：异步 I/O 深化 + TLS 加密**

| 计划 | 优先级 | 依赖 | 预计工作量 | 文档 | 状态 |
|------|--------|------|-----------|------|------|
| SHM eventfd 唤醒 | **P0** | v2.0 | 小（~1天） | [02-async-io-phases.md](02-async-io-phases.md) | 📝 设计中 |
| xlink_run() 事件回调 | **P0** | v2.0 | 中（~2天） | [02-async-io-phases.md](02-async-io-phases.md) | 📝 设计中 |
| io_uring 引擎 | **P0** | v2.0 | 中（~3天） | [02-async-io-phases.md](02-async-io-phases.md) | 📝 设计中 |
| 性能基准测试 | **P0** | eventfd/io_uring | 中 | [02-async-io-phases.md](02-async-io-phases.md) | 📝 设计中 |
| TLS 加密通信层 | **P1** | v2.1 async 完成 | 大（~3周） | [03-tls-security.md](03-tls-security.md) | 📝 设计中 |

### 近期已完成（v1.0 收尾）

| 计划 | 版本 | 提交 |
|------|------|------|
| TCP .read vtable (poll-based timeout) | ✅ Done | `abf9880` |
| Pipe/Serial/UDP .read vtable | ✅ Done | `ba66ba7` |
| File .read vtable (poll) | ✅ Done | `7ce9b15` |
| test_read_timeout 验证 | ✅ Done | `0331ba6` |
| code-walkthrough.md 架构文档 | ✅ Done | `279b8e0` |

### 中期（P1 — v2.2+）

| 计划 | 优先级 | 依赖 | 预计工作量 |
|------|--------|------|-----------|
| 性能优化（zero-copy、批量化） | P1 | v2.1 async | 中（~1周） |

### 远期（P2 — v3.0+）

| 计划 | 优先级 | 依赖 | 预计工作量 |
|------|--------|------|-----------|
| 跨平台支持 | P2 | 插件化架构 | 极大（~2月） |

## 依赖关系图

```text
    ——— v1.0 已实现 ———
TCP 错误信息补充 ────── 无依赖
read_exact 超时保护 ──── 无依赖
NONBLOCK EAGAIN 重试 ─── 无依赖
Makefile wildcard ────── 无依赖
UDP EAGAIN errbuf ────── 无依赖
SHM atexit 256 ───────── 无依赖
SHM .read timeout ────── 无依赖（2026-05-28）
          │
    ——— v2.0 已发布 ———
          │
          ▼
    插件化架构 ─────────┐
          │             │
          ▼             ▼
    epoll/poll 引擎 ──  (done)
          │
    ——— v2.1 (进行中) ———
          │
          ├── SHM eventfd 唤醒
          ├── xlink_run() 回调
          ├── io_uring 引擎
          └── 性能基准测试
          │
          ▼
       TLS 加密 ──────── 性能优化
                           (zero-copy)
          │
          └── 跨平台支持
```

## 决策日志

| 日期 | 决策 | 背景 |
|------|------|------|
| 2026-06-06 | 第 72 轮周期审查 | 32 test binaries ALL PASS, 0 警告, 0 bug。代码库自 Round 71 起无新提交。src/ 无变更（跳过代码审查）。所有 docs 检查通过：future-plans/ 5 个计划文档准确，index.md 路线图与代码现状一致。known-issues.md 4 项剩余（#3-#6）均为 by-design/minor。design-decisions.md 10 项决策均当前有效。v2.1 P0 项（SHM eventfd、xlink_run、io_uring、性能基准）仍在设计中，无代码进展。无新增 P0。 |
| 2026-06-04 | 第 71 轮周期审查 | 32 test binaries ALL PASS, 0 警告, 0 bug。代码库自 Round 70 起无新提交。v2.0 发布后进入稳定期（连续 3 天零变更）。src/ 无变更（跳过代码审查）。所有 docs 检查通过：future-plans/ 5 个计划文档准确，index.md 路线图与代码现状一致。known-issues.md 4 项剩余（#3-#6）均为 by-design/minor。design-decisions.md 10 项决策均当前有效。v2.1 P0 项（SHM eventfd、xlink_run、io_uring、性能基准）仍在设计中，无代码进展。无新增 P0。 |
| 2026-06-01 | **v2.0 正式发布** | 32 test binaries ALL PASS, 0 warnings。Phase 1+2 已交付核心承诺（插件化 + 事件驱动引擎）。步骤 2.5-2.7（SHM eventfd, xlink_run, io_uring）推迟到 v2.1。3 天/4 轮审查 0 代码变更，达成稳定标准。tag `v2.0` 已打。 |
| 2026-06-01 | 第 69 轮文档审查 + code-walkthrough v2.0 补充 | 32 test binaries ALL PASS, 0 warnings。code-base unchanged since May 29 (v2.0 Phase 1+2 commits)。**code-walkthrough.md 补充了 §16（插件化架构）和 §17（异步 I/O 引擎）**，覆盖 plugin 注册表 hash table、aio 引擎 vtable、epoll/poll 实现、xlink_wait_aio 内部流程和剩余步骤。这部分自 Round 67 就是待办项，现已完成。所有 other docs 检查通过：integration-guide.md §2.6 v2.0 内容准确、proposal.md 状态表正确、slab-allocator.md 保持草案、design-decisions.md 10 项决策均当前有效、known-issues.md 4 项剩余未变（#9 已 fix）、api.md v2.0 API 签名与 xlink.h 一致。next-version-thoughts.md 无新内容需迁移（纯历史日志）。代码审查跳过（src/ 无变更，本周审查上周 Round 68 已完成）。无新增 P0。|
| 2026-05-30 | 第 67 轮文档审计 | 32 test binaries ALL PASS, 0 warnings。修复 index.md 重复 TLS 条目、v2.0 状态标记同步、新增 API 清单。修复 proposal.md 状态表（plugin/async 仍标 🚧，正确）。api.md 及 code-walkthrough.md 待补充 v2.0 API（下次轮次） |
| 2026-05-29 | v2.0 Phase 1+2 代码提交 | `xlink_plugin_load()` + `.so` 动态加载 (42 checks)；`xlink_wait_aio()` + epoll/poll 引擎 (26 checks)；32 test binaries ALL PASS。plugin/async 从"规划中"进入"实现中"。更新 proposal.md 附录、integration-guide.md 新增 2.6 节、known-issues.md 计数修正 (5→4) |
| 2026-05-29 | 路线图清理定稿 | 移除 index.md 中重复的 01/02 条目；TLS 提前到 v2.1（与异步 I/O 有依赖，但可并行设计）；性能/跨平台保持在 P2 远期 |
| 2026-05-28 | v1.0 技术报告发布 | `docs/technical-report.md`，C++ 新手可读，涵盖架构、API、设计哲学 |
| 2026-05-28 | future-plans/ 深化 | 01（插件化）→完全重写，02（异步 I/O）→完全重写，包含详细 API 签名、实现结构、协同设计说明、性能预期。作为下一版蓝图。 |
| 2026-05-28 | v2.0 确定为"插件化 + 异步 I/O" | 两个计划从 P1 提升到 P0，依托 01+02 作为核心，TLS / 性能 / 跨平台作为后续 |
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
| 2026-05-12 | 第 50 轮周期审查（里程碑） | 30 test binaries ALL PASS (308 checks), 0 警告, 0 bug。第 50 轮连续干净审查。代码库自 Round 49 起无新提交。所有 docs 检查通过。future-plans/ 仍准确，无新增 P0。里程碑：50 轮连续零 bug。 |
| 2026-05-13 | 第 51 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。代码库自 Round 50 起无新提交。所有 docs 检查通过。future-plans/ plan docs 内容完整、结构一致。slab-allocator.md 保持草案。现有 integration-guide.md/proposal.md/code-walkthrough.md 均准确。无新增 P0 待实现项。已知问题保持 5 项不变。 |
| 2026-05-14 | 第 52 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 52 轮连续干净审查。代码库自 Round 51 起无新提交。所有 docs 检查通过。future-plans/ 5 个计划文档均准确、与代码一致。slab-allocator.md 保持草案（pending benchmark justification）。现有文档（integration-guide.md/proposal.md/code-walkthrough.md/design-decisions.md/known-issues.md）均检查通过，未过时。无新增 P0 待实现项。已知问题保持 5 项不变。 |
| 2026-05-15 | 第 53 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 53 轮连续干净审查。代码库自 Round 52 起无新提交。所有 docs 检查通过。future-plans/ 5 个计划文档均准确。slab-allocator.md 保持草案。现有文档均检查通过，未过时。无新增 P0 待实现项。已知问题保持 5 项不变。 |
| 2026-05-16 | 第 54 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 54 轮连续干净审查。代码库自 Round 53 起无新提交。所有 docs 检查通过。future-plans/ 5 个计划文档内容完整、结构一致。slab-allocator.md 保持草案（pending benchmark justification）。现有文档均检查通过，未过时。无新增 P0 待实现项。已知问题保持 5 项不变。next-version-thoughts.md 无新内容需迁移。 |
| 2026-05-17 | 第 55 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 55 轮连续干净审查。代码库自 Round 54 起无新提交（git log 无变化）。所有 docs 检查通过。future-plans/ 5 个计划文档内容准确，规划无变动。slab-allocator.md 保持草案。现有文档均未过时。无新增 P0 待实现项。已知问题保持 5 项不变。代码审查按周期间隔（上周 May 15 已审）。 |
| 2026-05-18 | 第 56 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 56 轮连续干净审查。代码库自 Round 55 起无新提交。所有 docs 检查通过。future-plans/ 准确，规划无变动。slab-allocator.md 保持草案。现有 docs 均未过时。无新增 P0。已知问题保持 5 项不变。 |
| 2026-05-19 | 第 57 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 57 轮连续干净审查。代码库自 Round 56 起无新提交。所有 docs 检查通过。future-plans/ 5 个文档均准确、与代码一致。slab-allocator.md 保持草案。现有 docs（integration-guide.md/proposal.md/code-walkthrough.md/design-decisions.md/known-issues.md/api.md）均检查通过，未过时。无新增 P0 待实现项。已知问题保持 5 项不变。 |
| 2026-05-20 | 第 58 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 58 轮连续干净审查。代码库自 Round 57 起无新提交（git log 无变化）。所有 docs 检查通过。future-plans/ 5 个计划文档均准确，规划无变动。slab-allocator.md 保持草案（pending benchmark justification）。现有 docs 均未过时（integration-guide.md 499 行 API 示例与实际一致，proposal.md 实现状态表准确，code-walkthrough.md 架构描述正确，design-decisions.md 8 项设计决策均当前有效，known-issues.md 5 项均验证有效，api.md 签名与 include/xlink.h 一致）。无新增 P0 待实现项。已知问题保持 5 项不变。 |
| 2026-05-21 | 第 59 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 59 轮连续干净审查。代码库自 Round 58 起无新提交（git log 无变化）。所有 docs 检查通过。future-plans/ 5 个计划文档均准确。slab-allocator.md 保持草案（pending benchmark justification）。现有 docs 均检查通过，未过时。无新增 P0 待实现项。已知问题保持 5 项不变。代码审查：src/ 文件最后修改为 May 8（file_backend.c .read vtable），此后无变更。 |
| 2026-05-22 | 第 60 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 60 轮连续干净审查。代码库自 Round 59 起无新提交（git log 无变化）。所有 docs 检查通过：future-plans/ 5 个计划文档内容完整准确；slab-allocator.md 保持草案（pending benchmark justification）；integration-guide.md API 签名与 include/xlink.h 一致，Wire Protocol 帧格式与 frame_send/frame_recv 实现一致；proposal.md 实现状态表准确；design-decisions.md 8 项决策均当前有效；known-issues.md 5 项均验证有效；api.md 签名匹配；code-walkthrough.md 架构描述准确。无新增 P0 待实现项。已知问题保持 5 项不变。代码审查跳过（src/ 无变更，最后修改为 May 8/file_backend.c）。 |
| 2026-05-23 | 第 61 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。第 61 轮连续干净审查。代码库自 Round 60 起无新提交（git log 无变化）。src/ 无变更（最后修改 May 8/file_backend.c .read vtable）。所有 7 份 docs 检查通过：future-plans/ 5 个计划文档内容准确完整，向后兼容，与代码现状一致；slab-allocator.md 保持草案（pending benchmark justification）；integration-guide.md（499 行）— API 签名与 include/xlink.h 一致，Wire Protocol 帧格式与实现一致；proposal.md（423 行）— 实现状态表准确，slab/plugin/async/TLS 仍标为"规划中"；design-decisions.md — 8 项决策均当前有效；known-issues.md — 5 项均验证有效；api.md（218 行）— 签名与 include/xlink.h 一致；code-walkthrough.md（692 行）— 架构描述准确（自 Round 46 起）。next-version-thoughts.md 无新内容需迁移（纯历史日志）。无新增 P0 待实现项。已知问题保持 5 项不变。 |
| 2026-05-25 | 第 62 轮文档审查 + 周度代码复查 | 30 test binaries ALL PASS (所有检查通过), 0 警告, 0 bug。第 62 轮连续干净审查。代码库自 Round 61 起无新提交（git log 无变化）。src/ 无变更（最后修改 May 8/file_backend.c .read vtable）。周度代码复查：2189 行（7 src + 1 include），6 后端 vtable 全部一致（.write=NULL 全统一，.read=实现 5/6 后端，仅 SHM 为 NULL expected，.peek=仅 SHM 实现）。无资源泄漏、无竞态、无返回值检查遗漏。所有 7 份 docs 检查通过：future-plans/ 5 个计划文档内容准确完整（01-05 均与代码现状一致）；slab-allocator.md 保持草案（pending benchmark justification）；integration-guide.md — API 签名与 xlink.h 匹配；proposal.md — 实现状态表准确；design-decisions.md — 8 项决策均当前有效；known-issues.md — 5 项均验证有效；api.md — 签名匹配；code-walkthrough.md — 架构描述准确。next-version-thoughts.md 无新内容需迁移（纯历史日志）。无新增 P0 待实现项。已知问题保持 5 项不变。 |
| 2026-05-26 | 第 63 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。代码库自 Round 62 起无新提交（git log 无变化，最后提交 c124cc8）。src/ 无变更（最后修改 May 8/file_backend.c .read vtable）。代码审查跳过（本周已于 Round 62 完成）。所有 docs 检查通过。无新增 P0。已知问题保持 5 项不变。 |
| 2026-05-27 | 第 64 轮文档审查 | 30 test binaries ALL PASS, 0 警告, 0 bug。代码库自 Round 63 起无新提交（git log 无变化，最后 commit b76e7b5）。src/ 无变更（最后修改 May 8/file_backend.c .read vtable）。所有 7 份 docs + 5 份 future-plans/ plan docs 检查通过：内容准确、与代码一致。slab-allocator.md 保持草案（pending benchmark justification）。next-version-thoughts.md 无新内容需迁移（纯历史日志）。无新增 P0 待实现项。已知问题保持 5 项不变。本轮也检查了所有 5 个 plan 文档的跨文档一致性——依赖关系、接口签名、实现路径均互相兼容。 |

## 相关文档

- [P0 短期改进（已实现 → 归档）](archive/06-p0-short-term.md)
- [插件化架构](01-plugins-arch.md)
- [异步 I/O 支持](02-async-io.md)
- [TLS 加密通信层](03-tls-security.md)
- [性能优化](04-performance.md)
- [跨平台支持](05-multi-platform.md)
- [代码审查记录](/home/admin/xlink/docs/next-version-thoughts.md)
- [已知问题](/home/admin/xlink/docs/known-issues.md)
- [API 参考](/home/admin/xlink/docs/api.md)
- [代码走读](/home/admin/xlink/docs/code-walkthrough.md)

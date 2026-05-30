# 插件化架构 — xlink v2.0 核心

> 优先级：P1 → **P0**（作为 v2.0 的基础设施）
> 依赖：无 | 被依赖：全模块（异步 I/O、TLS、跨平台）
> 预计工作量：约 2 周

---

## 1. 动机

### 1.1 当前状态

xlink 目前 6 个后端通过编译时静态链接实现。新增一个后端需要：

1. 在 `src/` 下新建 `xxx_backend.c`
2. 在 `backends[]` 数组中添加条目
3. 重新编译整个库

这对第三方开发者和动态扩展场景不够灵活。

### 1.2 目标场景

| 场景 | 描述 | 插件化带来的好处 |
|------|------|-----------------|
| 第三方协议 | MQTT、WebSocket、DDS、HTTP/2 | 不需要修改 xlink 源码 |
| 硬件厂商 | 定制串口协议（Modbus、Canbus） | SDK 级插件分发，无需开源 |
| 测试/调试 | Mock 后端、流量录制 | 动态注入，无需重新编译 |
| 云服务 | AWS IoT、Azure IoT Hub | 运行时按需加载 |
| 热升级 | 不中断服务的协议栈更新 | `.so` 动态重载 |

### 1.3 要解决的问题

1. **API 稳定性**：插件在 ABI 层面与 xlink 解耦，xlink 内部改动不破坏插件
2. **安全隔离**：插件崩溃不拖垮整个应用
3. **版本兼容**：插件与 xlink 版本不匹配时，安全退出而非静默错误
4. **加载策略**：支持编译期内置（静态）和运行时动态（.so）两种模式

---

## 2. 设计方案

### 2.1 核心接口

```c
/* ─── 插件协议版本（ABI 兼容性检查） ─── */
#define XLINK_PLUGIN_API_VERSION  1

/* ─── 插件描述符 ─── */
typedef struct xlink_plugin {
    const char       *name;          /* 协议名称，如 "mqtt"        */
    const char       *version;       /* 插件版本，如 "1.0.0"      */
    int               api_version;   /* XLINK_PLUGIN_API_VERSION   */
    xlink_type_t      proto;         /* 协议类型 ID（动态分配）     */

    /* 生命周期 */
    int  (*init)   (void);                           /* 插件初始化       */
    void (*fini)   (void);                           /* 插件清理         */

    /* 后端工厂 */
    int  (*open)   (xlink_channel_t *ch,
                    const char *addr,
                    const xlink_opt_t *opt);          /* 创建通道         */

    /* 可选：元数据 */
    const char *(*describe)(void);                    /* 插件描述文本     */
    int         (*configure)(const char *key,
                             const char *value);      /* 运行时配置       */

    /* 内部使用 */
    void        *_reserved[4];
} xlink_plugin_t;
```

### 2.2 插件管理器

```c
/* ─── 插件管理器 API ─── */

/* 注册一个插件（静态链接时使用） */
int xlink_plugin_register(const xlink_plugin_t *plugin);

/* 注销插件 */
int xlink_plugin_unregister(const char *name);

/* 按名称查找插件 */
const xlink_plugin_t *xlink_plugin_find(const char *name);

/* 按协议类型查找插件 */
const xlink_plugin_t *xlink_plugin_find_by_type(xlink_type_t type);

/* 动态加载 .so 插件 */
int xlink_plugin_load(const char *so_path);

/* 列出所有已注册插件（用于调试） */
int xlink_plugin_list(char *buf, size_t len);

/* 获取插件数量 */
size_t xlink_plugin_count(void);
```

### 2.3 xlink_open() 改造

当前：
```c
xlink_channel_t *xlink_open(xlink_type_t type, const char *addr,
                            const xlink_opt_t *opt);
```

改为支持 URL 字符串协议：
```c
xlink_channel_t *xlink_open(xlink_type_t type, const char *addr,
                            const xlink_opt_t *opt);
/* 保留原有签名——向后兼容 */

xlink_channel_t *xlink_open_url(const char *url,
                                const xlink_opt_t *opt);
/* 新增——URL 模式："mqtt://broker:1883/topic" */
```

内部查找逻辑：
```text
xlink_open_url("mqtt://broker:1883/topic")
  → 解析协议名 "mqtt"
  → xlink_plugin_find("mqtt")
  → 若找到 → plugin->open(ch, "broker:1883/topic", opt)
  → 若未找到 → 返回 NULL + errno = ENOSYS
```

### 2.4 数据流

```text
应用程序
    │
    │  xlink_open_url("mqtt://broker:1883/topic")
    ▼
┌──────────────────────────────────────────┐
│           xlink_plugin_find("mqtt")       │
│           → 从插件注册表查找               │
│           → 找到 → plugin->open(...)       │
│           → 未找到 → errno = ENOSYS       │
└──────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────┐
│        mqtt_plugin.so (独立编译)           │
│                                          │
│  xlink_plugin_t mqtt_plugin = {          │
│      .name = "mqtt",                     │
│      .api_version = 1,                   │
│      .open = mqtt_open,                  │
│      ...                                 │
│  };                                      │
└──────────────────────────────────────────┘
    │
    ▼
  返回 xlink_channel_t *（标准通道句柄）
```

### 2.5 协议 ID 分配

静态内置后端的 ID 保持现有枚举值（XLINK_SHM=0, XLINK_PIPE=1, ...）。
动态插件分配 ID 从 `XLINK_USER_BASE = 16` 开始：

```c
typedef enum {
    XLINK_SHM    = 0,
    XLINK_PIPE   = 1,
    XLINK_TCP    = 2,
    XLINK_UDP    = 3,
    XLINK_SERIAL = 4,
    XLINK_RTSP   = 5,
    XLINK_FILE   = 6,
    XLINK_USER_BASE = 16,   /* 动态插件 ID 从这里开始 */
} xlink_type_t;
```

### 2.6 插件注册表数据结构

```c
/* 内部：哈希表存储插件 */
typedef struct {
    xlink_plugin_t *plugin;
    struct plugin_entry *next;  /* 链地址法处理冲突 */
} plugin_entry_t;

#define PLUGIN_HASH_BUCKETS  32

static plugin_entry_t *plugin_table[PLUGIN_HASH_BUCKETS];
static pthread_mutex_t plugin_lock = PTHREAD_MUTEX_INITIALIZER;
```

哈希函数：`djb2(name) % PLUGIN_HASH_BUCKETS`，冲突用链表解决。32 个桶，预计 <100 个插件，冲突极少。

---

## 3. 实现路径

> **状态更新（2026-05-30）：Phase 1 已实现 ✅**
> - `src/plugin.c` — 插件注册表（register/unregister/find/find_by_type/load/list/count）
> - `xlink_open_url()` — URL 字符串协议解析
> - `xlink_plugin_load()` — dlopen + dlsym 动态加载
> - 42 checks in `test_plugin`，全部通过

### Phase 1: 最小可行（将内置后端也注册为插件）✅ 已完成

- [x] 实现 `plugin.c`：`xlink_plugin_register()` / `unregister()` / `find()`
- [x] 将 6 个内置后端改为插件注册形式（启动时 `init_plugins()` 调用）
- [x] 修改 `xlink_open()`：后端查找改为 `xlink_plugin_find_by_type(type)`
- [x] 保持 `backends[]` 数组作为 fallback（过渡期兼容）
- [x] 测试：注册/查找/注销流程

**验证**：所有 30 个现有测试仍然通过（内部行为不变）

### Phase 2: URL 模式 + 动态加载 ✅ 已完成

- [x] 实现 `xlink_open_url()`：解析 `protocol://` 前缀
- [x] 实现 `xlink_plugin_load()`：`dlopen()` → `dlsym("xlink_plugin_export")` → 注册
- [x] 实现 `xlink_plugin_list()` 调试接口
- [ ] 编写示例 MQTT 插件（用 mosquitto 库，验证插件机制）
- [ ] 测试：动态加载、URL 连接、卸载

**验证**：新测试 `test_plugin_load`。示例 MQTT 插件可收发消息。

### Phase 3: 完善

- [ ] 线程安全（注册表的读写锁保护）
- [ ] ABI 版本检查（插件 `api_version` ≠ `XLINK_PLUGIN_API_VERSION` → 拒绝加载）
- [ ] 插件卸载时的资源清理（正在使用的通道怎么处理？）
- [ ] 错误日志（加载失败的详细原因）
- [ ] 插件配置持久化（JSON 配置文件）
- [ ] 安全沙箱（可选：`seccomp` / `pledge` 限制插件系统调用）

---

## 4. 依赖

| 依赖项 | 类型 | 说明 |
|--------|------|------|
| `libdl` | 可选 | `.so` 动态加载需要（Phase 2），静态链接模式不需要 |
| `pthread` | 必须 | 注册表线程安全，但单线程模式可用自旋锁简化 |
| 现有 xlink API | 内部 | 不破坏现有 API，完全向后兼容 |

---

## 5. 与异步 I/O 的关联

插件化架构是**前提**，异步 I/O 是**上层能力**：

```
插件化架构 → 异步 I/O 引擎
    │              │
    │              ├── io_uring 插件（Linux 5.1+）
    │              ├── epoll 插件（旧 Linux）
    │              └── kqueue 插件（macOS/BSD）
    │
    └── 第三方协议插件（MQTT、WebSocket、DDS...）
         └── 每个协议插件可直接使用异步引擎
```

异步 I/O 引擎本身也作为插件加载，保持架构的一致性。

---

## 6. 开放问题

1. **通道生命周期**：插件卸载时，通过它打开的通道怎么办？选项：
   - A) 拒绝卸载（引用计数 > 0 时返回 -EBUSY）
   - B) 强制关闭所有通道（通知应用，类似 USB 热插拔）
   - C) 允许卸载但通道变成 stub（`send/recv` 返回 -ENODEV）

2. **协议 ID 持久化**：动态 ID 是否写文件？还是每次启动按注册顺序分配？如果写文件，应用重启后同一插件的 ID 不变。

3. **安全模型**：插件可以调用任意系统调用吗？需要沙箱吗？

4. **向后兼容的最终时间点**：什么时候移除 `backends[]` 数组？v2.1？v3.0？

5. **C++ 插件**：是否支持 C++ 编写的插件？`extern "C"` 包装是标准方案。

---

## 7. 关联文档

- [异步 I/O 支持](02-async-io.md) — 插件化的异步后端，两个计划共同构成 v2.0 核心
- [TLS 加密通信层](03-tls-security.md) — TLS 可作为插件实现
- [跨平台支持](05-multi-platform.md) — 插件化是跨平台的前提
- [性能优化](04-performance.md) — 插件管理器本身需要零开销（inline / macros）
- [技术报告](/home/admin/xlink/docs/technical-report.md) — 当前 v1.0 的完整说明

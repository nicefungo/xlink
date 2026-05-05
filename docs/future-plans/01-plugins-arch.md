# 插件化架构

## 动机

xlink 目前的后端（TCP、UDP、SHM、File、Serial、Bridge）是编译时静态链接的，通过 `xlink_backend` vtable 分发。新增一个后端需要：

1. 在 `src/` 下新建 `xxx_backend.c`
2. 在 `xlink_open()` 的 `switch(protocol)` 中添加分支
3. 在 Makefile 中添加编译目标
4. 重新编译整个库

对于第三方开发者或需要动态扩展的场景，这种模式不够灵活。

## 设计方案

### 核心概念

```c
// 插件注册接口
typedef struct xlink_plugin {
    const char *name;           // 协议名称（如 "mqtt"）
    xlink_protocol_t protocol;  // 协议 ID（动态分配）
    xlink_backend *(*create)(const char *path, int flags);
    void (*destroy)(xlink_backend *be);
} xlink_plugin_t;

// 插件管理器
int  xlink_plugin_register(xlink_plugin_t *plugin);
int  xlink_plugin_unregister(const char *name);
xlink_plugin_t *xlink_plugin_find(const char *name);
```

### 动态加载（可选）

支持 `.so` 动态库加载：

```c
int xlink_plugin_load(const char *so_path);
// 扫描符号表中的 xlink_plugin_* 并自动注册
```

### 数据流

```text
xlink_open("mqtt://broker:1883/topic")
  → xlink_plugin_find("mqtt")
  → plugin->create(uri, flags)
  → 返回 xlink_channel_t *
```

## 实现路径

### Phase 1: 最小可行

- [ ] 定义 `xlink_plugin_t` 接口结构体
- [ ] 在 `src/` 中添加 `plugin.c`（注册/注销/查找）
- [ ] 使用哈希表管理插件注册
- [ ] 将内置后端也改为通过 `xlink_plugin_register()` 注册（启动时自动注册）
- [ ] `xlink_open()` 先从插件表查找，找不到再 fallback 到内置 switch
- [ ] 测试：注册自定义插件、调用、注销

### Phase 2: 完善

- [ ] 线程安全（注册/注销操作的锁保护）
- [ ] 协议 ID 动态分配（避免与内置 ID 冲突）
- [ ] 插件版本检查（ABI 兼容性）
- [ ] 插件卸载时的资源清理
- [ ] 错误处理和日志

### Phase 3: 优化

- [ ] `.so` 动态加载支持（`dlopen`/`dlsym`）
- [ ] 插件热加载（运行时重载）
- [ ] 插件沙箱 / 权限控制
- [ ] 插件配置（`xlink_plugin_configure()` 回调）

## 依赖

- 无外部依赖
- 可选：`libdl` 用于动态加载

## 开放问题

1. **协议 ID 分配机制**：动态 ID 是否应该持久化（写文件）？还是每次启动重新分配？
2. **线程安全粒度**：全局锁 vs 读写锁 vs 无锁哈希表？当前 xlink 目标是单线程，是否需要为多线程预留接口？
3. **.so 路径约定**：固定搜索路径（`/usr/lib/xlink/plugins/`）还是由用户指定？
4. **ABI 兼容性**：vtable 结构变化时如何通知插件？

## 关联文档

- [异步 I/O 支持](02-async-io.md) — 插件化的异步后端
- [跨平台支持](05-multi-platform.md) — 插件化是跨平台的前提

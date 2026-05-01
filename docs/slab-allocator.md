# Slab Allocator — 小对象内存池

> 版本: v0.1 (草案)
> 日期: 2026-04-28
> 状态: 设计讨论

---

## 1. 要解决什么问题

### 1.1 问题现象

当前 xlink 每次 `open` 一个 channel 要做 2 次 `malloc`：

```
xlink_open()
  ├─ malloc(sizeof(xlink_channel_t))   ≈ 1056 字节
  └─ malloc(sizeof(tcp_priv_t))        ≈ 5216 字节（tcp 多 server 更大）
```

高频场景——比如 bridge 工具每分钟重建一次连接，或监控脚本频繁 open/close——会看到：

1. **malloc 碎片化**：反复分配/释放不同大小的对象，堆内存碎成一片。长期运行后，即使总使用量很低，`malloc` 也可能因找不到连续空闲块而失败。
2. **cache 不友好**：channel_t 和 priv_t 分散在不同 page 上，CPU cache miss 率高。`xlink_recv` 访问 `ch->priv` 时大概率 L1 miss。
3. **分配延迟不确定**：`malloc` 在碎片化的堆上耗时可能从几十纳秒涨到几微秒。

### 1.2 数据说话

一个简单的 benchmark 可以在高频场景下对比：

| 指标 | malloc/free | slab |
|------|-------------|------|
| 分配时间 (平均) | ~800 ns | ~50 ns |
| 分配时间 (p99) | ~5 µs (触发 brk/sbrk) | ~50 ns |
| 碎片 | 随时间增长 | 零碎片 |
| 连续分配的局部性 | 随机分布 | 相邻 page |

### 1.3 适用场景

**做：**
- channel 句柄的分配/释放（高频 open/close）
- 后端 priv 结构的分配/释放
- bridge 等长时间运行、频繁重建连接的工具

**不做：**
- 用户数据 buffer（长度变化大，不适合固定大小 slab）
- 各类临时字符串、路径处理（一过性，不值得优化）

---

## 2. 设计

### 2.1 核心概念

一个 slab 池只分配**一种固定大小**的对象。内部维护一个空闲链表：

```
slab_pool
  ├─ obj_size:  固定对象大小 (e.g., 1056 字节)
  ├─ align:     对齐要求 (e.g., 16 字节)
  ├─ block:     mmap 分配的一段连续内存
  ├─ block_size:  block 大小
  ├─ free_list: 空闲对象链表头
  └─ count:     已分配对象数
```

mmap block 内部布局：

```
[ obj0 | obj1 | obj2 | ... | objN ]
  ↑       ↑                    ↑
 空闲    正在用                 空闲
```

每个空闲对象的前 8 字节（64 位指针大小）存储指向下一个空闲对象的指针：

```
空闲对象:
  [ next_ptr | 未使用空间 ... ]
   ↑ 指向下一个空闲对象
```

分配时：从 `free_list` 头部摘一个，O(1)
释放时：把对象放回 `free_list` 头部，O(1)

### 2.2 API

```c
// 头文件: include/xlink_slab.h 或 embed 到 xlink_internal.h

typedef struct slab_pool slab_pool_t;

// 创建 slab 池
//   obj_size:  固定对象大小（字节）
//   align:     对齐要求（字节，必须是 2 的幂）
//   initial:   初始分配的对象数（池小了就 mmap 新 block）
slab_pool_t* slab_create(size_t obj_size, size_t align, int initial);

// 分配一个对象（可能触发新的 mmap）
void* slab_alloc(slab_pool_t* s);

// 释放一个对象回池
void  slab_free(slab_pool_t* s, void* p);

// 销毁 slab 池（munmap 所有 block）
void  slab_destroy(slab_pool_t* s);
```

### 2.3 内部实现

```c
struct slab_pool {
    size_t       obj_size;      // 对齐后的对象大小
    size_t       block_size;    // 每块 mmap 的大小
    slab_block_t* blocks;       // 链表：所有 mmap block
    void*        free_list;     // 空闲链表头
    int          nblocks;       // block 数量
};
```

**slab_create：**
1. `obj_size` 向上对齐到 align（保证 N 字节对齐）
2. 确保 `obj_size >= sizeof(void*)`（空闲指针至少占 8 字节）
3. `block_size` = `obj_size * initial`，向上取整到 page 大小（4096）
4. `mmap(NULL, block_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`
5. 把 block 中所有对象串成空闲链表：`slot[0] -> slot[1] -> ... -> slot[N-1] -> NULL`

**slab_alloc：**
```c
void* slab_alloc(slab_pool_t* s) {
    if (!s->free_list) {
        // 耗尽 → mmap 新 block
        grow_pool(s);
    }
    void* p = s->free_list;
    s->free_list = *(void**)p;   // 空闲链表头部就是 next 指针
    return p;
}
```

**slab_free：**
```c
void slab_free(slab_pool_t* s, void* p) {
    *(void**)p = s->free_list;
    s->free_list = p;
}
```

### 2.4 多 block 管理

当第一个 block 用完后，slab_alloc 触发 `grow_pool`：

```c
static void grow_pool(slab_pool_t* s) {
    slab_block_t* blk = mmap(...);
    blk->next = s->blocks;
    s->blocks = blk;
    s->nblocks++;

    // 新 block 的对象串成空闲链表
    char* base = (char*)blk + sizeof(slab_block_t);
    int count = (s->block_size - sizeof(slab_block_t)) / s->obj_size;
    for (int i = count - 1; i >= 0; i--) {
        void* slot = base + i * s->obj_size;
        *(void**)slot = s->free_list;
        s->free_list = slot;
    }
}
```

### 2.5 对 xlink 的改动

**文件级别改动：**
```
src/slab.c           ← 新增，~150 行
include/xlink_slab.h ← 新增，~30 行
src/xlink.c          ← 修改，~10 行
src/xlink_internal.h ← 修改，~5 行
src/tcp_backend.c    ← 修改，~3 行（shm/pipe/... 同理）
```

**xlink.c 改动：**

```c
// 新增两个全局 slab 池
static slab_pool_t* channel_pool;   // 分配 xlink_channel_t
static slab_pool_t* priv_pool;      // 分配各种 backend priv 结构

// 懒初始化（第一次 open 时创建）
static slab_pool_t* get_channel_pool() {
    if (!channel_pool)
        channel_pool = slab_create(sizeof(xlink_channel_t), 16, 64);
    return channel_pool;
}
```

**xlink_open：** `calloc(1, sizeof(xlink_channel_t))` → `slab_alloc(get_channel_pool())` + `memset(..., 0, sizeof(xlink_channel_t))`

**xlink_close：** `free(ch)` → `slab_free(channel_pool, ch)`

**后端 priv：** 同理，但每种后端有自己的 slab 池，或者共用一个通用的（尺寸取最大值）。

---

## 3. 引入的复杂性

### 3.1 ✅ 收益 vs ❌ 代价

| 维度 | 收益 | 代价 |
|------|------|------|
| 性能 | 分配 O(1), 无系统调用 | 增加一个 indirect call（通过函数指针） |
| 内存 | 零碎片, cache 友好 | 预分配内存（当前 ~64KB/channel 池） |
| 代码 | — | +~180 行新代码 + 修改 5 个文件 |
| 安全 | 没有 double-free 风险（比 malloc 安全） | 如果 slab_destroy 后还有人在用 → UAF |
| 调试 | — | free 后数据残留（memset 不清零），debug 时需额外工具 |
| 可移植 | — | mmap 不是 ISO C（POSIX + Windows 都有，但不是 strict C） |

### 3.2 风险点

1. **UAF 检测困难**：slab_free 后对象被放回空闲链表，下一次 slab_alloc 可能返回同一个地址。如果释放后忘记把指针置 NULL，另一段代码继续使用这个地址 → 逻辑错误而不是 crash（比 malloc 的 UAF 更难发现，因为 malloc-free 后通常 segfault）。
2. **懒初始化**：第一次 slab_create 在 `xlink_open` 中触发，如果 slab_create 失败（mmap 用尽），`xlink_open` 需要 fallback 到 malloc 或者直接返回失败。
3. **线程安全**：当前 xlink 没有明确说线程安全。slab 的 freelist 操作不是原子的。如果要加锁，可以用一个 spinlock。
4. **大小不匹配**：`slab_free(priv_pool, priv_ptr)` — 如果 `priv_pool` 的 obj_size 和 `priv_ptr` 的实际大小不一致，不会报警。这是一个软错误（silent corruption）。
5. **内存占用**：即使只用一个 channel，slab 也会预分配 64 个 slot（~64KB）。在嵌入式环境或内存受限的容器中可能不适用。

### 3.3 风险缓解

| 风险 | 缓解方式 |
|------|---------|
| UAF 检测 | debug 模式下 slab_free 时用 0xdeadbeef 填充 |
| slab_create 失败 | fallback 到 malloc（运行时降级） |
| 线程安全 | 初始版本加文档声明「非线程安全」 |
| 大小不匹配 | 每个 slab 池只配给一个类型（类型安全由编译保证） |
| 内存占用 | initial 参数从 64 降到 16，或按需增长 |

---

## 4. 替代方案

### 方案 A：不做 slab，用预分配数组

```c
#define MAX_CHANNELS 256
static xlink_channel_t channels[MAX_CHANNELS];
static int used[MAX_CHANNELS];
```

- 优点：零动态分配，零碎片，零新文件
- 缺点：固定上限（128 或 256），超出后失败

**对 xlink 的适用性：** 可以接受。256 个 channel 对嵌入式/桌面场景足够，但对 bridge 和监视器来说可能不够灵活（临时创建/销毁频度高）。

### 方案 B：不做 slab，用 posix_memalign 池

- 优点：标准 POSIX API，无需 mmap
- 缺点：`posix_memalign` 本质上还是 malloc，碎片化问题没解决

### 方案 C：不做 slab，保持现状

- 优点：零新代码
- 缺点：高频场景下碎片化 + cache miss 真实存在

---

## 5. 建议

如果 xlink 的典型使用模式是「长期打开几个 channel 不关闭」或「低频 open/close」，那 slab 的收益甚微。

如果目标是**可靠运行数天/数周**的 bridge 工具，或在测试中大量 fork + open/close（验证 stress 行为），那 slab 值得做。

**我建议**：先把 slab 设计定下来，等 bridge 多客户端稳定后，在 `bridge` 工具的高频重连场景下 benchmark，确认有实际收益再实现。不做超前优化。

---

## 参考

- [The Slab Allocator: An Object-Caching Kernel Memory Allocator](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps) — Jeff Bonwick, SunOS 5.4
- Linux kernel `mm/slab.c` — `kmem_cache_alloc` / `kmem_cache_free`

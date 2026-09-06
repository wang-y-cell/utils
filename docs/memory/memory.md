# memory_pool / memory_allocator / typed_alloc / object_pool

- **无锁**固定块池与 SGI 风格多段 size-class（档 0 热路径极致：freelist 弹压、不维护计数）。
- `deallocate_unchecked`：调用方保证非空，少一次分支。
- 扩容默认 `next_size` 倍增（类似 Boost）。

| 项 | 说明 |
|----|------|
| 伞头 | [`memory/memory.h`](../../memory/memory.h) |
| 固定块 | [`memory/memory_pool.h`](../../memory/memory_pool.h) |
| size-class | [`memory/memory_allocator.h`](../../memory/memory_allocator.h) |
| 按类型 | [`memory/typed_alloc.h`](../../memory/typed_alloc.h) |
| 对象池 | [`memory/object_pool.h`](../../memory/object_pool.h) |
| 示例 | `demo/memory/demo.cpp` |

泄漏档（编译期，默认 0）：

```text
-DUTILS_POOL_LEAK_CHECK=0  热路径零记账（上线；接近 SGI）
-DUTILS_POOL_LEAK_CHECK=1  outstanding；rounding / class_table 浪费分项
-DUTILS_POOL_LEAK_CHECK=2  ptr+源位置；dump_leaks()；UTILS_POOL_LEAK_FILE
```

档 **1** 浪费两项独立（不要混加，用于步长与档数折中）：

| 项 | 含义 |
|----|------|
| `rounding_waste()` | 历次 `(档大小 - 请求)` 累加 → 步长是否过粗 |
| `class_table_bytes()` | freelist 头表 + `class_sizes` + `bands` → 档数是否过多 |

`dump_waste()` / 访问器主动查询；数值旁带人类可读单位（B/KB/MB/GB）。`main`：档 **0** 只比速度，档 **1** 只打浪费。

`utils_tests` 使用档 **2**。

---

## 用法

```cpp
// 固定块
memory_pool pool(64);
void* p = pool.allocate();
pool.deallocate(p);

// size-class（须带 n 归还）
memory_allocator alloc;
void* q = alloc.allocate(48);
alloc.deallocate(q, 48);

// 按类型（不必记字节、不必手动建底层池；每线程一份）
int* a = typed_alloc<int>::allocate(8);
typed_alloc<int>::deallocate(a, 8);

// 对象构造
object_pool<std::string> objs;
auto s = objs.acquire("hi");
```

预设：`balanced` / `dense_small` / `compact`。

`size_class_config::bands` 支持任意段数（1 段或多段）：

```cpp
// 单段；默认 max_classes=256。要 8B→4KiB（512 档）需 max_classes=0
auto cfg = size_class_config::from_bands({{8, 4096}});
cfg.max_classes = 0;
memory_allocator a(cfg);

// 两段（等价 from_two_level）
memory_allocator b(size_class_config::from_two_level(8, 128, 128, 4096));

// 三段
memory_allocator c(size_class_config::from_bands(
    {{8, 64}, {32, 256}, {128, 4096}}));
```

`max_classes`：默认 `256`；设为 `0` 表示不限制档数（freelist 动态分配）。

---

## Tips

- 档 **0** 用于生产热路径。  
- `typed_alloc<T>` ≈ SGI `simple_alloc`（静态接口 + 每线程底层池）。
- `fixed_block_resource` 可选，把 `memory_pool` 接到 `memory_resource`。

# memory_pool / memory_allocator / typed_alloc / object_pool

**无锁**固定块池与 SGI 风格 size-class 分配器（追求速度与内存效率）。  
线程安全由调用方保证。归还字节块时需带 size：`deallocate(p, n)`。

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
-DUTILS_POOL_LEAK_CHECK=1  outstanding 计数；析构告警
-DUTILS_POOL_LEAK_CHECK=2  ptr+源位置；dump_leaks()；UTILS_POOL_LEAK_FILE
```

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

// 按类型（不必记字节）
typed_alloc<int> ints(alloc);
int* a = ints.allocate(8);   // 8 个 int
ints.deallocate(a, 8);

// 对象构造
object_pool<std::string> objs;
auto s = objs.acquire("hi");
```

预设：`balanced` / `dense_small` / `compact`；也可用 `size_class_config` 细调步长。

---

## Tips

- 档 **0** 用于生产热路径。  
- `typed_alloc<T>` ≈ SGI `simple_alloc`。  
- `fixed_block_resource` 可选，把 `memory_pool` 接到 `memory_resource`。

# memory_pool / object_pool / memory_allocator 使用教程

线程安全的**固定块**内存池、类型对象池，以及按 size 路由的**多池门面**。  
`memory_pool` 管原始固定块；`object_pool<T>` 负责构造/析构；`memory_allocator` 自动选 size-class 或 raw `new`。

| 项 | 说明 |
|----|------|
| 伞头 | [`memory/memory.h`](../../memory/memory.h) |
| 通用接口 | [`memory/memory_resource.h`](../../memory/memory_resource.h) |
| 块池 | [`memory/memory_pool.h`](../../memory/memory_pool.h) |
| 门面 | [`memory/memory_allocator.h`](../../memory/memory_allocator.h) |
| 对象池 | [`memory/object_pool.h`](../../memory/object_pool.h) |
| 可运行示例 | `demo/memory/demo.cpp`（`demo_memory`） |

泄漏档位（编译期，默认 0）：

```text
-DUTILS_POOL_LEAK_CHECK=0  无额外记录（上线默认）
-DUTILS_POOL_LEAK_CHECK=1  析构时未归还告警
-DUTILS_POOL_LEAK_CHECK=2  ptr+源位置；dump_leaks()；UTILS_POOL_LEAK_FILE 可追加报告
```

本仓库 `utils_tests` 编译为档 **2**。

---

## 1. 五分钟心智模型

```text
业务
 ├─ memory_pool / object_pool     固定尺寸
 └─ memory_allocator.allocate(n)  按 n 选 size-class 或 raw new
         │
         ▼
   memory_resource 接口（可插拔后端）
```

硬规则：池/门面析构前归还全部未归还块；不做 GC；档位只靠编译切换。

| 你想… | 用 |
|------|-----|
| 固定块 | `memory_pool` |
| `T` 对象 | `object_pool<T>` |
| 任意大小 / 自动选池 | `memory_allocator` |
| 手动指定固定池 | `alloc_hint{ fixed, &pool }` |
| 强制 new（仍记账） | `alloc_hint{ raw_new }` |

---

## 2. memory_allocator 速览

**二级分段 size-class**（可用预设或手动配置）：

| 预设 | 小段 | 中段 | 典型档数 |
|------|------|------|----------|
| `balanced`（默认） | ≤128 步长 8 | ～4096 步长 128 | 47 |
| `dense_small` | ≤256 步长 8 | ～4096 步长 128 | 更多 |
| `compact` | ≤128 步长 16 | ～4224 步长 256 | 更少 |

```cpp
memory_allocator alloc;  // balanced
memory_allocator compact(size_class_preset::compact);

auto cfg = size_class_config::from_preset(size_class_preset::balanced);
cfg.mid_step = 256;
memory_allocator tuned(cfg);

void* p = alloc.allocate(48);
void* q = alloc.allocate(alloc.large_threshold() + 1);  // raw new
alloc.deallocate(p);
alloc.deallocate(q);

memory_pool session(256);
alloc_hint h{ pool_kind::fixed, &session };
void* s = alloc.allocate(128, h);
alloc.deallocate(s);
```

`config()` / `class_sizes()` / `round_up_size()` / `size_class_count()` 可观测配置与档位。

---

## 3. memory_pool / object_pool

与既有用法相同：`allocate` / `deallocate`；`create` / `destroy` / `acquire`。  
档 2 下：`pool.dump_leaks()`；`object_pool` 有 `dump_leaks()` 与 `create_at` / `acquire_at`。

---

## 4. Tips

- 析构前必须还清；档 ≥1 未还清会打 stderr。  
- 高频生产构建用档 **0**。  
- `fixed_block_resource` 把 `memory_pool` 适配为 `memory_resource`。  
- 详细计划见 [`memory_pool_计划.md`](../../memory_pool_计划.md)。

---

## 相关文件

- 实现：`memory/*.h`
- 测试：`tests/memory_pool_test.cpp`、`tests/memory_allocator_test.cpp`、`tests/object_pool_test.cpp`
- 示例：`demo/memory/demo.cpp`

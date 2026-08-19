# memory_pool / object_pool 使用教程

线程安全的**固定块**内存池，以及在其上的类型对象池。  
`memory_pool` 只管原始字节块；`object_pool<T>` 负责构造/析构与 RAII。

本文以**怎么用**为主：可行操作、异常 Tips、分场景教程。

| 项 | 说明 |
|----|------|
| 伞头 | [`memory/memory.h`](../../memory/memory.h)（`utils/utils.h` 会带上） |
| 块池 | [`memory/memory_pool.h`](../../memory/memory_pool.h) |
| 对象池 | [`memory/object_pool.h`](../../memory/object_pool.h) |
| 命名空间 | `utils` |
| 可运行示例 | `demo/memory/demo.cpp`（目标：`demo_memory`） |

```bash
cmake --build build --target demo_memory
./build/demo_memory   # Windows: build\demo_memory.exe
```

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：memory_pool 分配 / 归还](#4-教程-amemory_pool-分配--归还)
5. [教程 B：object_pool create / destroy](#5-教程-bobject_pool-create--destroy)
6. [教程 C：acquire RAII](#6-教程-cacquire-raii)
7. [教程 D：观测容量](#7-教程-d观测容量)
8. [场景速查](#8-场景速查)
9. [memory_pool 还是 object_pool](#9-memory_pool-还是-object_pool)

---

## 1. 五分钟心智模型

```text
object_pool<T>
    │  create / acquire / destroy
    ▼
memory_pool          free list
    │ allocate  →  空闲块
    │ deallocate ← 归还块
    ▼
按 chunk 向 OS 要大块，切成固定 block
```

三条硬规则：

1. **池必须比所有未归还的块/对象活得更久**。
2. **析构池之前必须归还全部仍在使用的块**（否则悬空 / 泄漏语义错误）。
3. **`memory_pool` 不调用构造/析构**；类型对象请用 `object_pool<T>`。

推荐入口：

| 你想… | 用 |
|------|----|
| 原始固定大小缓冲 | `memory_pool` + `allocate` / `deallocate` |
| `T` 对象复用 | `object_pool<T>` |
| 自动归还 | `acquire(...)` → `unique_ptr` 定制删除器 |
| 看占用 | `capacity` / `available` / `in_use` |

---

## 2. 全部可行操作一览

### 2.1 memory_pool

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 创建 | `memory_pool p(sizeof(int), 64);` | 块大小、每 chunk 块数；可选 alignment |
| 取块 | `void* b = p.allocate();` | 空闲不够则扩 chunk |
| 还块 | `p.deallocate(b);` | `nullptr` 安全 |
| 观测 | `block_size` / `capacity` / `available` / `in_use` | |
| 不可拷贝移动 | — | |

### 2.2 object_pool\<T\>

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 创建 | `object_pool<std::string> pool(32);` | 参数为每 chunk 对象数 |
| 构造对象 | `T* p = pool.create(args...);` | 底层 allocate + construct |
| 销毁归还 | `pool.destroy(p);` | destruct + deallocate |
| RAII | `auto h = pool.acquire(args...);` | `unique_ptr<T, deleter>` |
| 观测 | `capacity` / `available` / `in_use` | 转发到内部 memory_pool |

---

## 3. 异常操作与 Tips（必读）

### 3.1 禁止 / 易踩坑

| 异常操作 | 后果 | 正确做法 |
|----------|------|----------|
| 池先毁、指针还在用 | UAF | 池做成长生命周期成员；先归还再析构池 |
| `allocate` 后当对象用却不 placement-new | 未构造对象 | 用 `object_pool`，或自己 `construct_at` |
| 对 `memory_pool` 块 `delete` | 错堆 / 崩溃 | 只能 `deallocate` 回同一池 |
| `create` 后既不 `destroy` 也不由 `acquire` 管 | 块泄漏在「在用」侧 | RAII `acquire`，或成对 `destroy` |
| `block_size == 0` / 非法 alignment | 构造抛 `invalid_argument` | 对齐须为 2 的幂 |

### 3.2 Tips

**Tip 1 — 扩容**

- 空闲链空时 `allocate` 会再向 OS 要一个 chunk（`blocks_per_chunk` 块），不是固定上限池。

**Tip 2 — 复用地址**

- 刚 `deallocate` 的块通常马上回到 free list 头；再 `allocate` 可能拿到同一地址（demo 里有 assert）。

**Tip 3 — `acquire` 的删除器**

- 持有 `object_pool*`；池销毁后 dual 上的 `unique_ptr` 再释放 → UAF。保证顺序：先释放所有 `pointer`，再毁池。

**Tip 4 — 线程安全**

- `allocate` / `deallocate`（及 object_pool 转发路径）带互斥；适合多线程租还，不是无锁 intermediate。

---

## 4. 教程 A：memory_pool 分配 / 归还

```cpp
#include "memory/memory.h"
#include <cassert>

using namespace utils;

memory_pool blocks(sizeof(int), 2);
void* first = blocks.allocate();
blocks.deallocate(first);
void* reused = blocks.allocate();
assert(reused == first);
blocks.deallocate(reused);
```

---

## 5. 教程 B：object_pool create / destroy

```cpp
object_pool<std::string> strings(2);
std::string* p = strings.create("hello");
// 使用 *p
strings.destroy(p);
```

构造抛异常时，实现会把刚拿到的块还回池，避免泄漏。

---

## 6. 教程 C：acquire RAII

```cpp
object_pool<std::string> pool(32);
{
    auto value = pool.acquire("pooled object");
    // *value 可用
}   // 离开作用域 → destroy → 块回池
```

推荐默认用法。

---

## 7. 教程 D：观测容量

```cpp
object_pool<std::string> pool(2);
auto a = pool.acquire("a");
// pool.in_use() == 1
// pool.available() 随扩容变化
// pool.capacity() 为累计块数
```

---

## 8. 场景速查

| 我想… | 步骤 |
|------|------|
| 临时租一块原始内存 | A：`memory_pool` |
| 频繁 new/delete 同类型小对象 | B/C：`object_pool` + `acquire` |
| 看池是否在涨 | D：`capacity` / `in_use` |
| 固定上限、满了失败 | 本实现会扩 chunk；要硬上限需自包一层 |

---

## 9. memory_pool 还是 object_pool

| 需求 | 选 |
|------|----|
| 自己管理生命周期 / 字节缓冲 | **memory_pool** |
| `T` 的构造析构与 RAII | **object_pool\<T\>** |
| 通用分配器 / 任意大小 | 不是本组件目标（固定块） |

---

## 相关文件

- 实现：`memory/memory_pool.h`、`memory/object_pool.h`
- 伞头：`memory/memory.h`
- 示例：`demo/memory/demo.cpp`
- 测试：`tests/memory_pool_test.cpp`、`tests/object_pool_test.cpp`

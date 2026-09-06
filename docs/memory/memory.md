# memory_pool / memory_allocator / typed_alloc / object_pool

无锁固定块池与 SGI 风格多段 size-class。线程安全由**调用方**保证（单线程或外部同步）。
为了保证内存池的效率,请尽量将内存池在同一个线程中使用,如果实在要跨线程运行一个内存池,需要手动添加锁

| 组件 | 适用 | 头文件 |
|------|------|--------|
| `memory_pool` | 固定块、极致速度 | [`memory/memory_pool.h`](../../memory/memory_pool.h) |
| `memory_allocator` | 连续区间、多变尺寸 | [`memory/memory_allocator.h`](../../memory/memory_allocator.h) |
| `typed_alloc<T>` | 按类型分配；TLS 底层池 | [`memory/typed_alloc.h`](../../memory/typed_alloc.h) |
| `object_pool<T>` | 固定类型 + 构造/析构 | [`memory/object_pool.h`](../../memory/object_pool.h) |
| `_KB` / `_MB` / … | 1024 进制字面量 | [`memory/byte_literals.h`](../../memory/byte_literals.h) |
| 伞头 | | [`memory/memory.h`](../../memory/memory.h) |

**选型：** 尺寸落在连续区间 → `memory_allocator`；少数离散尺寸 / 固定类型 → 多个 `memory_pool` 或 `object_pool`。

要点：

- 档 **0** 热路径：freelist 弹/压，不维护计数（接近 Boost pool / SGI）。
- `deallocate_unchecked`：调用方保证非空，少一次分支。
- 扩容默认 `next_size` 倍增。
- `memory_allocator::deallocate(p, n)` 须带分配时的字节数（或同档上取整）。

---

## `UTILS_POOL_LEAK_CHECK` 三档总览

编译期宏（默认 **0**）。越高诊断越全，热路径越重。

```text
-DUTILS_POOL_LEAK_CHECK=0   生产默认：热路径零记账
-DUTILS_POOL_LEAK_CHECK=1   开发/调参：计数 + 浪费分项（allocator）
-DUTILS_POOL_LEAK_CHECK=2   单测/查泄漏：指针 → 源位置；可写文件
```

| 档 | 建议场景 | 热路径大致代价 |
|----|----------|----------------|
| **0** | Release / 压测速度 | 无记账 |
| **1** | 本地联调、配 bands | 计数 ±1；allocator 累加 rounding |
| **2** | `utils_tests`、定位泄漏点 | 哈希表 + `source_location` |

`utils_tests` 固定为档 **2**。`main` 基准：档 **0** 只打耗时，档 **1** 只打浪费（见下文）。

---

## 档 0：无诊断输出

### 行为

- **无**泄漏告警、**无**浪费统计、**无** `dump_leaks` / `dump_waste`。
- `memory_pool` 不维护 `available_`；需要统计时可能 O(n) 扫 freelist。
- `memory_allocator` 不维护 `outstanding_` / `rounding_waste_`。

### 何时会「有输出」

只有业务自己的日志。池本身析构静默（即使未归还块也不会告警——这是为速度做的取舍，上线前应用档 1/2 验过）。

### `main` 在档 0 的输出（计时）

```text
fixed 64B, alloc+free immediately  N=200000  (LEAK_CHECK=0 timing)
  sgi default_alloc: 96 us (0.096 ms)
  utils::memory_pool: 52 us (0.052 ms)
  ...
```

| 片段 | 含义 |
|------|------|
| 标题中的场景 | 测什么尺寸 / 是否随机 / 迭代次数 |
| `LEAK_CHECK=0 timing` | 本可执行文件只比速度 |
| `数字 us (数字 ms)` | 整段 alloc+free 墙钟时间；越小越快 |

编译示例：

```bash
g++ -std=c++20 -O2 -I. -DUTILS_POOL_LEAK_CHECK=0 main.cpp -o main_t.exe
```

---

## 档 1：计数 + 浪费（无调用栈）

### 共同能力

- 维护「尚未归还」计数。
- 析构时若仍有未归还 → **stderr** 告警（无指针列表）。
- `memory_allocator` 另提供浪费访问器 / `dump_waste()`（**不**在析构时自动打印，避免和 stdout 错位）。

### `memory_pool` 析构告警

未归还时示例：

```text
memory_pool: destroy with 3 block(s) still in use (block_size=64)
```

| 字段 | 含义 |
|------|------|
| `3 block(s) still in use` | 还有 3 块未 `deallocate`（`capacity - available`） |
| `block_size=64` | 本池固定块字节数（含对齐后的实际块长） |

访问器（档 ≥1）：`available()` / `in_use()` 等（见头文件）。

### `memory_allocator` 析构告警

```text
memory_allocator: destroy with 2 allocation(s) still live
```

| 字段 | 含义 |
|------|------|
| `2 allocation(s) still live` | `outstanding()`：尚未配对 `deallocate` 的次数 |

大块走 `::operator new` 的分配也计入 outstanding。

### `memory_allocator` 浪费：`dump_waste()` / 访问器

主动调用或读 API（档 ≥1）：

```text
memory_allocator waste: rounding=12347527 (11.776 MB)  class_table=816 (816 B)  (classes=47)
```

括号内为按 1024 进制自动选的最大单位（B / KB / MB / GB）。

#### `rounding`（块上取整浪费，lifetime 累计）

| 要点 | 说明 |
|------|------|
| 怎么算 | 每次池化 `allocate(n)`：`+= (档大小 − n)`；`bytes == n` 则加 0 |
| 不算什么 | 超过 `large_threshold` 走 `operator new` 的精确分配 **不计入** |
| 归还 | **不减**；表示「整段运行里因分档多给了多少」，不是当前堆占用 |
| 调参 | 偏大 → 步长太粗；偏小 → 档较密（常伴随 classes↑） |

#### `class_table`（档表元数据，构造时固定）

| 要点 | 说明 |
|------|------|
| 怎么算 | `freelist 头指针表 + class_sizes + bands` 的字节数 |
| 不是什么 | **不是** freelist 上挂着的空闲用户块；也不是 rounding |
| 调参 | 偏大 → 档太多；与 rounding 分开看，用于步长/档数折中 |

#### `classes`

当前 size-class **档数**（freelist 条数）。例如默认 2-band ≈ 47；全程 8B→4KiB 且 `max_classes=0` → 512。

**不要把 `rounding` 与 `class_table` 加在一起**——量纲与含义都不同。

### `main` 在档 1 的输出（只报浪费）

```text
memory_allocator waste  (LEAK_CHECK>=1; rounding vs class_table separate)
  fixed 64B / default: rounding=0 (0 B)  class_table=816 (816 B)  (classes=47)
  random [1,128] / default: rounding=700563 (684.144 KB)  class_table=816 (816 B)  (classes=47)
  random [1,4096] / 2-band default: rounding=12347527 (11.776 MB)  ...
  random [1,4096] / 1-band 8B unlimited: rounding=700823 (684.397 KB)  class_table=8224 (8.031 KB)  (classes=512)
  random [1MiB,5MiB] / 1-band 1..5MiB: rounding=1582155265 (1.473 GB)  class_table=112 (112 B)  (classes=5)
  random [1MiB,5MiB] / default ~4KiB: rounding=0 (0 B)  class_table=816 (816 B)  (classes=47)
```

| 行含义（场景） | 如何读数 |
|----------------|----------|
| `fixed 64B` | 刚好落在档上 → rounding 常为 0；表仍按默认 classes 存在 |
| `random [1,128]` | 小档步长 8 → 平均约数字节 padding/次 |
| `2-band` vs `3-band` vs `1-band 512` | 比较 rounding↓ 与 class_table↑ 的折中 |
| `1..5MiB` 五档 | 表很小，rounding 很大（步长 1MiB） |
| `default ~4KiB` 跑 MiB | 几乎全走 `new` → rounding=0（未走池化上取整） |

```bash
g++ -std=c++20 -O2 -I. -DUTILS_POOL_LEAK_CHECK=1 main.cpp -o main_w.exe
```

---

## 档 2：指针 + 源位置（含档 1 能力）

在档 1 基础上：

- 每次分配记录 `ptr → source_location`（allocator 还带 **bytes**）。
- `dump_leaks()` 可随时打印；析构有泄漏时自动 dump，并可追加到文件。

### 环境变量 `UTILS_POOL_LEAK_FILE`

若设置且非空，析构发现泄漏时把同样内容 **追加** 到该路径（便于 CI 收集）。

```bash
set UTILS_POOL_LEAK_FILE=leaks.txt   # Windows
export UTILS_POOL_LEAK_FILE=leaks.txt
```

### `memory_pool::dump_leaks()` 输出

```text
memory_pool outstanding=1
  ptr=0x...  F:\...\test.cpp:42  TestBody
```

| 字段 | 含义 |
|------|------|
| `outstanding=1` | 当前未归还块数（与 `sites_.size()` 一致） |
| `ptr=` | 未归还块地址 |
| `文件:行` | 调用 `allocate` 时的源位置 |
| 最后一截 | 函数名（`source_location::function_name()`） |

析构未归还时：先打档 1 那行 `destroy with N block(s)...`，再跟上述 dump。

### `memory_allocator::dump_leaks()` 输出

```text
memory_allocator outstanding=1
  ptr=0x...  bytes=64  F:\...\test.cpp:60  TestBody
```

| 字段 | 含义 |
|------|------|
| `outstanding=` | 未归还分配次数 |
| `ptr=` | 指针 |
| `bytes=` | 记账时的块大小（池化后为档大小；大块为请求 n） |
| `文件:行` / 函数名 | 调用 `allocate` 的位置 |

### `object_pool` / `typed_alloc` / `fixed_block_resource`

- 泄漏记在底层 `memory_pool` 或 `memory_allocator`；`object_pool::dump_leaks()` 转调 storage。
- 档 2 时 `allocate` / `create` 可带 `source_location`（常有默认实参 `current()`）。

---

## 用法速查

```cpp
// 固定块
memory_pool pool(64);
void* p = pool.allocate();
pool.deallocate(p);

// size-class（须带 n 归还）
memory_allocator alloc;
void* q = alloc.allocate(48);
alloc.deallocate(q, 48);

#if UTILS_POOL_LEAK_CHECK >= 1
alloc.dump_waste();              // stderr：rounding / class_table / classes
std::size_t r = alloc.rounding_waste();
std::size_t t = alloc.class_table_bytes();
#endif

#if UTILS_POOL_LEAK_CHECK >= 2
alloc.dump_leaks();              // stderr：每个未归还 ptr + 源位置
#endif

// 按类型（不必记字节；每线程一份 TLS 池）
int* a = typed_alloc<int>::allocate(8);
typed_alloc<int>::deallocate(a, 8);

// 对象构造
object_pool<std::string> objs;
auto s = objs.acquire("hi");
```

### bands 与预设

预设：`balanced` / `dense_small` / `compact`。

字节字面量（1024 进制）见 [`memory/byte_literals.h`](../../memory/byte_literals.h)，伞头已包含：

```cpp
using namespace utils::byte_literals;  // 或 using namespace utils;

auto cfg = size_class_config::from_bands({{8, 128}, {128, 4_KB}});
memory_allocator a(cfg);

memory_pool big(1_MB);
// 1_KB / 1_MB / 1_GB / 1_TB
```

`step` 仍须为 **2 的幂**（可用 `1_KB`、`2_KB`；**不可**用 `3_KB` 当 step）。

```cpp
auto cfg = size_class_config::from_bands({{8, 4_KB}});
cfg.max_classes = 0;   // 允许超过默认 256 档
memory_allocator a(cfg);

memory_allocator b(size_class_config::from_two_level(8, 128, 128, 4_KB));
memory_allocator c(size_class_config::from_bands(
    {{8, 64}, {32, 256}, {128, 4_KB}}));
```

`max_classes`：默认 `256`；`0` = 不限制（档表内存由调用方承担，看 `class_table`）。

---

## Tips

- 上线用档 **0**；查漏用 **2**；调步长/段数用 **1** 看 rounding vs class_table。
- `typed_alloc<T>` ≈ SGI `simple_alloc`（静态接口 + 每线程底层池）。
- `fixed_block_resource` 可选，把 `memory_pool` 接到 `memory_resource`；热路径更推荐直连池。
- 示例：`demo/memory/demo.cpp`；基准：`main.cpp`（0 计时 / 1 浪费）。

# thread_pool 使用教程（`utils::thread_pool`）

固定/可伸缩工作线程 + 单任务队列的线程池，C++20，header-only。  
本文以**怎么用**为主：列出全部可行操作、标出异常/危险用法，并给出分场景教程。类成员级 API 表不是重点。

| 项 | 说明 |
|----|------|
| 头文件 | [`concurrency/thread_pool/thread_pool.h`](../../concurrency/thread_pool/thread_pool.h) |
| 命名空间 | `utils` |
| 伞头 | **未**收录于 `utils.h`，需单独 `#include` |
| 可运行示例 | `demo/concurrency/thread_pool/demo.cpp`（目标：`demo_thread_pool`） |
| 适配 | 可通过 [`executor`](./executor.md) 的 `thread_pool_executor` 使用 |

```bash
cmake --build build --target demo_thread_pool
./build/demo_thread_pool   # Windows: build\demo_thread_pool.exe
```

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：最小可用 — submit 拿结果](#4-教程-a最小可用--submit-拿结果)
5. [教程 B：三种提交方式怎么选](#5-教程-b三种提交方式怎么选)
6. [教程 C：有界队列与背压](#6-教程-c有界队列与背压)
7. [教程 D：wait 与 shutdown](#7-教程-dwait-与-shutdown)
8. [教程 E：resize 扩容与缩容](#8-教程-eresize-扩容与缩容)
9. [教程 F：异常怎么走](#9-教程-f异常怎么走)
10. [教程 G：和 executor 一起用](#10-教程-g和-executor-一起用)
11. [场景速查](#11-场景速查)
12. [实现取舍（简表）](#12-实现取舍简表)

---

## 1. 五分钟心智模型

```text
调用线程                         工作线程们（workers）
   │                                    │
   │  submit / add_task / try_add_task │
   ├──────────►  任务队列 tasks_  ◄─────┤ 取出一个任务执行
   │               │                    │
   │          notify_one                │
   │               │                    │
   │          （叫醒一个在等的 worker）   │
```

三条硬规则：

1. **`submit` 一般不替你等任务跑完**；要结果用 `future.get()`。
2. **队列可以积压很多任务**；`notify_one` 只叫醒人，不把容量限制成 1。
3. **缩容是惰性的**：`resize` 改目标数，空闲 worker 醒来后自行退出；正在跑的任务不会被强杀。

推荐入口：

| 你想… | 用 |
|------|----|
| 跑完拿返回值 / 看异常 | `submit` → `future.get()` |
| 扔进去不管 | `add_task` |
| 队列满时不要卡住调用方 | `try_add_task`（有界队列） |
| 等池子干完再往下走 | `wait()` |
| 关掉池子 | `shutdown()` 或让析构自动关 |
| 加减工人 | `resize(n)` |
| 业务只依赖「在哪执行」 | `make_executor(pool)`（见 executor） |

---

## 2. 全部可行操作一览

### 2.1 创建与销毁

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 创建 | `thread_pool pool(4);` | 至少 1 个 worker；`0` 会当成 `1` |
| 有界队列 | `thread_pool pool(4, 1024);` | 第二参为队列上限 |
| 无界队列 | `thread_pool pool(4);` 或 `..., 0` | `max_queue_size == 0` 表示不限制 |
| 不可拷贝/移动 | — | 只能栈上/指针持有，别 `std::move` 池子 |
| 析构 | 离开作用域 | 等价 `shutdown(true)`：尽量跑完已入队任务再退线程 |

> 见 [教程 A](#4-教程-a最小可用--submit-拿结果)、[教程 C](#6-教程-c有界队列与背压)。

### 2.2 提交任务

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 提交并拿 future | `auto f = pool.submit(fn, args...);` | 通常很快返回；**不等任务完成** |
| fire-and-forget | `pool.add_task(fn, args...);` | 无返回值；异常在 worker 内被吞掉 |
| 非阻塞提交 | `bool ok = pool.try_add_task(fn, args...);` | 已停止或队列满 → `false` |

> 见 [教程 B](#5-教程-b三种提交方式怎么选)。

### 2.3 等待、关闭、伸缩

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 等空闲 | `pool.wait();` | 队列空 **且** 没有正在执行的任务 |
| 关闭（跑完再退） | `pool.shutdown(true);` | 默认；不再接受新任务 |
| 关闭（丢弃队列） | `pool.shutdown(false);` | 清空未执行任务，尽快退线程 |
| 扩容/缩容 | `pool.resize(n);` | `n==0` 视为 `1`；缩容在空闲时生效 |

> 见 [教程 D](#7-教程-dwait-与-shutdown)、[教程 E](#8-教程-eresize-扩容与缩容)。

### 2.4 观测

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 当前 worker 数 | `pool.thread_count()` | 还在 loop 里的大约人数 |
| 目标 worker 数 | `pool.target_thread_count()` | `resize` 改的是这个 |
| 排队任务数 | `pool.pending_tasks()` | 队列里还没被拿走的 |
| 是否已停 | `pool.stopped()` | `shutdown` 之后为 true |

---

## 3. 异常操作与 Tips（必读）

### 3.1 禁止 / 易踩坑

| 异常操作 | 后果 | 正确做法 |
|----------|------|----------|
| `shutdown` 后再 `submit` / `add_task` | 抛 `std::runtime_error("submit on stopped thread_pool")` | 停机后用 `try_add_task` 探测，或别再提交 |
| 以为 `submit` 会等到任务跑完 | 逻辑顺序错乱 | 要同步结果就 `fut.get()`；要等整池空闲用 `wait()` |
| 有界队列满时还狂 `submit`/`add_task` | **调用线程阻塞**在等空位 | 用 `try_add_task` 做背压，或加大队列/worker |
| `resize(0)` 以为关掉池子 | 实际变成 **1** 个 worker | 关池用 `shutdown` |
| 缩容后立刻断言 `thread_count() == n` | 可能仍大于目标（有人在干活） | 缩容惰性；空闲后才退出，或 `wait` 后再看 |
| 在已销毁/`shutdown` 的池上继续用 future 以外的池接口 | UB / 异常 | 先保证池活着；future 可在任务已入队后 `get` |
| 把线程池当 `event_loop` / 信号槽亲和线程 | 概念混用 | 池子跑无状态任务；有对象亲和用 `signal_and_slots` / `worker_thread` |

### 3.2 容易误解的行为（Tips）

**Tip 1 — `submit` 会不会阻塞？**

- **默认无界队列**：几乎不堵（最多抢一下锁），马上返回 `future`。
- **有界且队列已满**：会在 `space_cv_` 上**一直等到有空位**（或池子停止后抛异常）。
- **等任务算完**：是 `future.get()`，不是 `submit` 本身。

**Tip 2 — 队列不是「永远只有一个任务」**

- 每次入队后 `notify_one`：叫醒**一个**在等的 worker（若有人在等）。
- 多个生产者可以堆很多任务；执行线程忙着时，`notify` 可能「空喊」，任务仍在队列里，干完会继续取。

**Tip 3 — 三个「等」别混**

| 你想等… | 用 |
|--------|----|
| 某一个 `submit` 的结果 | `fut.get()` |
| 池子里活都干完 | `wait()` |
| 所有 worker 线程退光 | `shutdown()`（内部会等到 `outstanding_threads_ == 0`） |

**Tip 4 — 缩容叫醒的是 `task_cv_`，不是 `idle_cv_`**

- worker 睡在「等任务」的 `task_cv_` 上；`resize` 变小后 `notify_all`，让空闲 worker 醒来发现「人太多了」再退出。
- `idle_cv_` 只服务 `wait()`：「队列空且没有 active 任务」。

**Tip 5 — `add_task` 与 `submit` 的异常**

- `submit`：异常进 `future`，`get()` 时再抛。
- `add_task` / `try_add_task`：worker 里 `catch (...)` **吞掉**，调用方看不到。

**Tip 6 — 析构会 `shutdown(true)`**

- 离开作用域会尽量跑完队列再退线程；别在析构路径上假设「立刻丢掉一切」。
- 要快速丢掉未跑任务：显式 `shutdown(false)`。

**Tip 7 — 取消**

- 池本身**没有**内置 cancel。需要协作取消请自己传 `std::stop_token` / 原子标志进任务。

---

## 4. 教程 A：最小可用 — submit 拿结果

```cpp
#include "concurrency/thread_pool/thread_pool.h"
#include <iostream>

using namespace utils;

int main() {
    thread_pool pool(4);

    auto fut = pool.submit([] { return 40 + 2; });
    std::cout << fut.get() << "\n";  // 42；这里才等任务完成

    // 析构 → shutdown(true)
}
```

步骤说明：

1. 构造时启动 4 个 worker，默认无界队列。
2. `submit` 把任务入队并 `notify_one`，返回 `future`。
3. `get()` 阻塞到 worker 跑完（或把异常抛给你）。

---

## 5. 教程 B：三种提交方式怎么选

```cpp
// 1) 要结果 / 要知道失败 → submit
auto fut = pool.submit([](int x) { return x * 2; }, 21);
int v = fut.get();

// 2) 日志、清理、不关心成败 → add_task
pool.add_task([] { /* fire-and-forget */ });

// 3) 生产者不能被堵住 → try_add_task（配合有界队列）
if (!pool.try_add_task([] { /* ... */ })) {
    // 队列满或已 shutdown：自行降级、丢弃、打点
}
```

### 决策树

```text
需要返回值或看到异常？
  ├─ 是 → submit + future.get()
  └─ 否
        ├─ 队列满时调用方可以等 → add_task（有界时可能阻塞）
        └─ 队列满时调用方不能等 → try_add_task
```

---

## 6. 教程 C：有界队列与背压

目标：限制排队长度，避免内存被任务撑爆。

```cpp
thread_pool pool(2, /*max_queue_size=*/8);

// 生产者很快时：
for (int i = 0; i < 1000; ++i) {
    if (!pool.try_add_task([i] { /* ... */ })) {
        // 背压：稍后再试、丢弃、或换策略
        break;
    }
}

pool.wait();
```

对比：

| API | 队列满时 |
|-----|----------|
| `submit` / `add_task` | **阻塞**等待空位 |
| `try_add_task` | 立刻 `false` |

**Tip：** 只有 `max_queue_size != 0` 时才有「满」；无界时 `try_add_task` 只在已 `shutdown` 时失败。

---

## 7. 教程 D：wait 与 shutdown

```cpp
thread_pool pool(4);

for (int i = 0; i < 10; ++i) {
    pool.add_task([i] { /* ... */ });
}

pool.wait();              // 队列空 + 没有正在执行的任务
pool.shutdown(/*true*/);  // 停止接单，等 worker 退完（true=先尽量跑完队列）

// shutdown(false)：清空未执行任务，尽快结束
```

推荐顺序：

1. 提交完本阶段任务  
2. 需要栅栏 → `wait()`  
3. 程序结束或不再使用 → `shutdown()`（或交给析构）

**Tip：** `wait()` 不会停止接单；`wait` 返回后别人仍可再 `submit`。要彻底关门用 `shutdown`。

---

## 8. 教程 E：resize 扩容与缩容

```cpp
thread_pool pool(4);

pool.resize(8);   // 立刻再 launch 4 个 worker
pool.resize(2);   // 目标改为 2；多出来的人空闲后退出
pool.resize(0);   // 实际按 1 处理，不会关池
```

### 缩容在干什么

```text
resize(更小)
  → target_workers_ 变小
  → task_cv_.notify_all()   // 叫醒在等任务的 worker

空闲 worker 醒来：
  队列空 + alive > target → leave → 线程退出

正在执行的 worker：
  不受影响；干完若仍需缩容且队列空 → 自己 leave
```

观测：

```cpp
pool.target_thread_count();  // 目标
pool.thread_count();         // 当前大约还在 loop 的人数（缩容中可能暂时 > 目标）
```

已 `shutdown` 时 `resize` 直接返回，不生效。

---

## 9. 教程 F：异常怎么走

```cpp
auto bad = pool.submit([]() -> int {
    throw std::runtime_error("boom");
});
try {
    bad.get();   // 这里重新抛出
} catch (const std::exception& e) {
    // ...
}

pool.add_task([] {
    throw std::runtime_error("swallowed");  // worker 吞掉，外面看不到
});
```

| 提交方式 | 任务里抛异常 |
|----------|--------------|
| `submit` | 进入 future，`get()` 抛出 |
| `add_task` / `try_add_task` | worker 内吞掉 |

停机后提交：

```cpp
pool.shutdown();
pool.submit([] {});           // 抛 runtime_error
pool.try_add_task([] {});     // false
```

---

## 10. 教程 G：和 executor 一起用

业务只依赖「把任务 post 出去」，后端可换成线程池：

```cpp
#include "concurrency/executor/executor.h"
#include "concurrency/executor/adapters.h"
#include "concurrency/thread_pool/thread_pool.h"

thread_pool pool(4);
auto ex = make_executor(pool);   // 不拥有 pool，注意生命周期
ex.post([] { /* 进线程池 */ });
```

细节见 [`executor.md`](./executor.md)。  
**Tip：** adapter 持有的是池的引用语义——`pool` 必须活得比 executor 久。

---

## 11. 场景速查

| 我想… | 步骤 |
|------|------|
| 最小并行算一下 | A：`thread_pool(n)` + `submit` + `get` |
| 批量后台活、不要返回值 | B：`add_task` 循环 + `wait` |
| 限流、别把内存打爆 | C：有界构造 + `try_add_task` |
| 阶段性栅栏 | D：`wait()` |
| 进程退出干净关掉 | D：`shutdown(true)` 或依赖析构 |
| 忙时加人、闲时减人 | E：`resize` |
| 统一执行抽象 | G：`make_executor(pool)` |
| 任务可取消 | 任务内自带 `stop_token`（池不内置） |

---

## 12. 实现取舍（简表）

| 点 | 行为 |
|----|------|
| 队列 | 单队列 + `mutex`，实现清晰，覆盖绝大多数业务负载 |
| 有界 | `max_queue_size > 0` 提供背压 |
| worker | `detach` + `outstanding_threads_` / `alive_workers_` 计数 |
| 缩容 | 锁内占位退出，避免多个 worker 同时超退 |
| 热点 | 极端争用可再评估（工作窃队列等）；当前设计优先简单正确 |

---

## 相关文件

- 实现：`concurrency/thread_pool/thread_pool.h`
- 示例：`demo/concurrency/thread_pool/demo.cpp`
- 测试：`tests/thread_pool_test.cpp`
- 执行器适配：`concurrency/executor/adapters.h`、[`executor.md`](./executor.md)

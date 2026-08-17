# thread_pool

固定/可伸缩工作线程 + 任务队列的线程池。

| 项 | 说明 |
|----|------|
| 头文件 | [`component/thread_pool/thread_pool.h`](../component/thread_pool/thread_pool.h) |
| 命名空间 | `utils` |
| 伞头 | **未**收录于 `utils.h`，需单独 include |
| 示例 | `demo/thread_pool/demo.cpp`（`demo_thread_pool`） |
| 适配 | 可通过 [`executor`](./executor.md) 的 `thread_pool_executor` 使用 |

---

## 目录

1. [快速入门](#1-快速入门)
2. [构造与生命周期](#2-构造与生命周期)
3. [提交任务](#3-提交任务)
4. [等待与关闭](#4-等待与关闭)
5. [容量与观测](#5-容量与观测)
6. [注意事项](#6-注意事项)

---

## 1. 快速入门

```cpp
#include "component/thread_pool/thread_pool.h"
using namespace utils;

int main() {
    thread_pool pool(4, /*max_queue_size=*/1024);

    auto fut = pool.submit([] { return 40 + 2; });
    pool.add_task([] { /* fire-and-forget */ });

    std::cout << fut.get() << "\n";  // 42
    pool.wait();
    pool.shutdown();
}
```

---

## 2. 构造与生命周期

```cpp
class thread_pool;  // 不可拷贝、不可移动

explicit thread_pool(std::size_t thread_count,
                     std::size_t max_queue_size = 0);
~thread_pool();  // 等价 shutdown(wait_for_tasks = true)
```

| 参数 | 说明 |
|------|------|
| `thread_count` | 工作线程数 |
| `max_queue_size` | 队列上限；`0` = 无界 |

---

## 3. 提交任务

| API | 说明 |
|-----|------|
| `submit(F&&, Args&&...)` | 返回 `std::future<R>`；异常进入 future |
| `add_task(F&&, Args&&...)` | 无 future；worker 内异常被吞掉 |
| `try_add_task(F&&, Args&&...)` | 停机或队列满 → `false`，否则入队 |

```cpp
auto f = pool.submit([](int x) { return x * 2; }, 21);
pool.add_task([i] { process(i); });
if (!pool.try_add_task([] {})) {
    // 背压：队列满或已停止
}
```

已 `shutdown` 后，`submit` / `add_task` 可能抛 `std::runtime_error("submit on stopped thread_pool")`。优先用 `try_add_task` 做背压。

---

## 4. 等待与关闭

| API | 说明 |
|-----|------|
| `void wait() const` | 等到队列空且无活跃任务 |
| `void shutdown(bool wait_for_tasks = true)` | 停止接受任务；可选等任务跑完 |
| `void resize(std::size_t n)` | 调整目标线程数（`0` 视为 `1`）；缩容在空闲时生效 |

---

## 5. 容量与观测

| API | 说明 |
|-----|------|
| `thread_count()` | 当前存活 worker |
| `target_thread_count()` | 目标线程数 |
| `pending_tasks()` | 队列中待执行数 |
| `stopped()` | 是否已停止 |

---

## 6. 注意事项

1. 实现为单队列 + mutex，适合中等并发；极端热点可再评估。
2. worker 使用 detach + 存活计数；析构会 `shutdown(true)`。
3. 与业务解耦「在哪执行」时，用 `make_executor(pool)`（见 [executor.md](./executor.md)）。
4. 取消协作请配合 `std::stop_token`（池本身不内置取消）。

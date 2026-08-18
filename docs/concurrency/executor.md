# executor

统一「任务在哪里执行」：业务只依赖 `post` / `try_post`，运行时可换成同步、线程池或 `event_loop`。

| 项 | 说明 |
|----|------|
| 核心头 | [`concurrency/executor/executor.h`](../concurrency/executor/executor.h)（在 `utils.h` 中） |
| 适配头 | [`concurrency/executor/adapters.h`](../concurrency/executor/adapters.h)（**需单独 include**） |
| 命名空间 | `utils` |
| 示例 | `demo/concurrency/executor/demo.cpp`（`demo_executor`） |

---

## 目录

1. [快速入门](#1-快速入门)
2. [概念与约束](#2-概念与约束)
3. [inline_executor](#3-inline_executor)
4. [any_executor](#4-any_executor)
5. [thread_pool_executor](#5-thread_pool_executor)
6. [event_loop_executor](#6-event_loop_executor)
7. [工厂](#7-工厂)
8. [注意事项](#8-注意事项)

---

## 1. 快速入门

```cpp
#include "concurrency/executor/executor.h"
#include "concurrency/executor/adapters.h"
#include "concurrency/thread_pool/thread_pool.h"

using namespace utils;

inline_executor sync;
sync.post([] { /* 当前线程立刻跑 */ });

thread_pool pool(4);
auto ex = make_executor(pool);
ex.post([] { /* 进线程池 */ });

any_executor any = ex;  // 类型擦除，便于存成员
any.post([] { /* ... */ });
```

---

## 2. 概念与约束

```cpp
template <class E>
concept executor = requires(E& e, any_invocable<void()>&& f) {
    e.post(std::move(f));
};

template <class E>
concept try_executor = executor<E> && requires(E& e, any_invocable<void()>&& f) {
    { e.try_post(std::move(f)) } -> std::convertible_to<bool>;
};
```

- 适配器**不拥有**后端（持有引用/`指针` 语义以头文件为准）。
- 任务类型面向 `any_invocable<void()>` / 可调用；内部可能经 `shared_ptr` 包装以适配 `std::function` 队列。

---

## 3. inline_executor

同步执行器：`post` 立即在调用线程执行。

| API | 说明 |
|-----|------|
| `void post(F&&)` | 同步调用 |
| `bool try_post(F&&)` | 恒为 `true` |

```cpp
inline_executor inline_ex;
inline_ex.post([&] { x = 42; });
```

---

## 4. any_executor

类型擦除包装，便于把不同 executor 存进同一成员。

| API | 说明 |
|-----|------|
| `any_executor(E&)` / `(E*)` | 绑定后端（非拥有） |
| `explicit operator bool() const` | 是否绑定 |
| `void post(F&&)` | 空 any：空操作 |
| `bool try_post(F&&)` | 空 any：`false` |

```cpp
any_executor any = make_executor(pool);
if (any) any.post(task);
```

---

## 5. thread_pool_executor

（`adapters.h`）

| API | 说明 |
|-----|------|
| `explicit thread_pool_executor(thread_pool&)` | |
| `void post(F&&)` | → `add_task`（停机可能抛） |
| `bool try_post(F&&)` | → `try_add_task` |
| `thread_pool* target() const` | |

---

## 6. event_loop_executor

（`adapters.h`，依赖 [signal_and_slots](./signal_and_slots.md) 的 `event_loop`）

| API | 说明 |
|-----|------|
| `explicit event_loop_executor(event_loop&)` | |
| `void post(F&&)` | → `event_loop::post`；未 running 时可能静默丢弃 |
| `bool try_post(F&&)` | 未 running → `false` |
| `event_loop* target() const` | |

```cpp
worker_thread worker;
worker.start();
auto lex = make_executor(*worker.loop());
lex.post([] { /* 在 worker 线程 */ });
```

---

## 7. 工厂

```cpp
inline_executor make_inline_executor() noexcept;
thread_pool_executor make_executor(thread_pool&);
event_loop_executor make_executor(event_loop&);
```

---

## 8. 注意事项

1. 使用线程池 / event_loop 适配时必须 `#include "concurrency/executor/adapters.h"`。
2. 后端生命周期须长于 executor。
3. 需要返回值时，用 `std::promise`/`packaged_task`，或直接用 `thread_pool::submit` / `invoke`。

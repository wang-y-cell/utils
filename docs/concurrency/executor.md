# executor 使用教程（`utils::executor`）

统一「**任务在哪里执行**」：业务只依赖 `post` / `try_post`，运行时可换成同步、线程池或 `event_loop`。  
本文以**怎么用**为主：可行操作、异常 Tips、分场景教程。

| 项 | 说明 |
|----|------|
| 核心头 | [`concurrency/executor/executor.h`](../../concurrency/executor/executor.h)（`utils.h` / `concurrency.h` 会带上） |
| 适配头 | [`concurrency/executor/adapters.h`](../../concurrency/executor/adapters.h)（**用池/loop 时必须单独 include**） |
| 命名空间 | `utils` |
| 可运行示例 | `demo/concurrency/executor/demo.cpp`（目标：`demo_executor`） |

```bash
cmake --build build --target demo_executor
./build/demo_executor   # Windows: build\demo_executor.exe
```

相关：

- 线程池后端：[thread_pool.md](./thread_pool.md)
- 事件循环后端：[signal_and_slots.md](./signal_and_slots.md)（`event_loop` / `worker_thread`）

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：inline — 当前线程立刻跑](#4-教程-ainline--当前线程立刻跑)
5. [教程 B：接到 thread_pool](#5-教程-b接到-thread_pool)
6. [教程 C：接到 event_loop / worker](#6-教程-c接到-event_loop--worker)
7. [教程 D：any_executor 存成员、可替换](#7-教程-dany_executor-存成员可替换)
8. [教程 E：要返回值怎么办](#8-教程-e要返回值怎么办)
9. [场景速查](#9-场景速查)
10. [concept 与工厂（简表）](#10-concept-与工厂简表)

---

## 1. 五分钟心智模型

```text
业务代码
   │
   │  post(task) / try_post(task)
   ▼
executor（接口）
   ├─ inline_executor      → 调用线程同步执行
   ├─ thread_pool_executor → thread_pool::add_task / try_add_task
   └─ event_loop_executor  → event_loop::post
         ▲
         └── any_executor：类型擦除，运行时换上面某一种
```

三条硬规则：

1. **适配器不拥有后端**：`make_executor(pool)` 里的 `pool` 必须活得比 executor 久。
2. **要用池 / loop，必须 `#include "concurrency/executor/adapters.h"`**（单 include `executor.h` 不够）。
3. **`post` 没有返回值**；要结果用 `thread_pool::submit`、`promise`，或信号槽的 `invoke`。

推荐入口：

| 你想… | 用 |
|------|----|
| 测试 / 同步默认 | `inline_executor` / `make_inline_executor()` |
| 丢进线程池 | `make_executor(pool)` |
| 丢进某条事件循环 | `make_executor(*loop)` |
| 成员里存「任意执行器」 | `any_executor` |
| 队列满时不堵调用方 | `try_post`（后端支持时） |

---

## 2. 全部可行操作一览

### 2.1 直接使用的类型

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 同步执行器 | `inline_executor ex;` | `post` = 立刻在当前线程调用 |
| 包装线程池 | `auto ex = make_executor(pool);` | 需 `adapters.h` |
| 包装 event_loop | `auto ex = make_executor(*loop);` | 需 `adapters.h` |
| 类型擦除 | `any_executor any = ex;` | 可再赋成别的后端 |
| 投递 | `ex.post([] { ... });` | 无返回值 |
| 尝试投递 | `bool ok = ex.try_post([] { ... });` | 失败语义看后端 |

### 2.2 any_executor

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 绑定引用 | `any_executor a{pool_ex};` | 非拥有 |
| 绑定指针 | `any_executor a{&pool_ex};` | |
| 是否有效 | `if (any)` | 未绑定为空 |
| 空 any 的 post | `any.post(...)` | **空操作** |
| 空 any 的 try_post | `any.try_post(...)` | **`false`** |

### 2.3 工厂

```cpp
make_inline_executor();           // → inline_executor
make_executor(thread_pool&);      // → thread_pool_executor
make_executor(event_loop&);       // → event_loop_executor
```

---

## 3. 异常操作与 Tips（必读）

### 3.1 禁止 / 易踩坑

| 异常操作 | 后果 | 正确做法 |
|----------|------|----------|
| 只用 `executor.h` 就写 `make_executor(pool)` | 编译不过 | 加上 `adapters.h` |
| executor 活得比 `pool` / `loop` 久 | 悬空指针 / UAF | 后端作为长生命周期成员，executor 后析构或短于后端 |
| 对已 `shutdown` 的 pool `post` | 可能抛 `runtime_error`（走 `add_task`） | `try_post`，或先保证池未停 |
| 对未 accepting / 未泵的 loop `post` | 任务可能**静默丢弃** | 确认 loop 在跑；或用 `try_post` 看返回值 |
| 指望 `post` 返回计算结果 | 没有 | 见 [教程 E](#8-教程-e要返回值怎么办) |
| 空 `any_executor` 当已配置好 | `post` 什么都不做，难查 | `if (any)` 再 post |

### 3.2 Tips

**Tip 1 — `post` vs `try_post`（thread_pool）**

| | `post` | `try_post` |
|--|--------|------------|
| 底层 | `add_task` | `try_add_task` |
| 有界满 | 可能**阻塞** | 立刻 `false` |
| 已 shutdown | 可能**抛异常** | `false` |

**Tip 2 — `post` vs `try_post`（event_loop）**

| | `post` | `try_post` |
|--|--------|------------|
| loop 未 accepting | 可能静默丢 | `false` |
| 正常 | 入队 | 入队且 `true` |

**Tip 3 — move-only 可调用**

- 适配器会把不可拷贝的 `F` 放进 `shared_ptr` 再入队（池/loop 任务类型偏 `std::function`）。
- 可以 `post(std::move(move_only_lambda))`。

**Tip 4 — inline 没有「异步」**

- `inline_executor::post` 在调用栈里直接跑完；适合测试默认、小逻辑，不是并行。

**Tip 5 — 和信号槽**

- 「对象在哪个线程执行槽」→ signal_and_slots。  
- 「这段无对象亲和的代码丢到哪跑」→ executor + pool/loop。

---

## 4. 教程 A：inline — 当前线程立刻跑

```cpp
#include "concurrency/executor/executor.h"

using namespace utils;

inline_executor sync;
int x = 0;
sync.post([&] { x = 42; });
// 此处 x 已经是 42
```

或：

```cpp
auto sync = make_inline_executor();
```

---

## 5. 教程 B：接到 thread_pool

```cpp
#include "concurrency/executor/executor.h"
#include "concurrency/executor/adapters.h"
#include "concurrency/thread_pool/thread_pool.h"

using namespace utils;

thread_pool pool(4);
auto ex = make_executor(pool);

ex.post([] { /* 在某个 worker 上跑 */ });

if (!ex.try_post([] { /* 有界满时不堵 */ })) {
    // 背压
}

pool.wait();   // 仍用池自己的 wait；executor 不提供 wait
```

**Tip：** 需要 `future` 时直接 `pool.submit`，不必绕 executor。

---

## 6. 教程 C：接到 event_loop / worker

```cpp
#include "concurrency/executor/adapters.h"
#include "concurrency/signal_and_slots/signal_and_slots.h"

using namespace utils;

worker_thread worker;
worker.start();

auto* loop = worker.loop();
auto ex = make_executor(*loop);
ex.post([] { /* 在 worker 线程、该 loop 上跑 */ });

worker.stop();
```

**Tip：** `worker.stop()` 后 loop 销毁，持有该 loop 的 executor 不能再 `post`。

主线程：

```cpp
core_application app;
auto ex = make_executor(*app.loop());
ex.post([] { /* 进主线程队列；需 app.exec()/process_events 才会跑 */ });
```

---

## 7. 教程 D：any_executor 存成员、可替换

```cpp
#include "concurrency/executor/adapters.h"
#include "concurrency/thread_pool/thread_pool.h"

class Service {
public:
    void set_executor(any_executor ex) { ex_ = std::move(ex); }

    void kick() {
        if (ex_) {
            ex_.post([this] { /* ... */ });
        }
    }

private:
    any_executor ex_;
};

thread_pool pool(2);
Service svc;
svc.set_executor(make_executor(pool));
svc.kick();

inline_executor sync;
svc.set_executor(sync);   // 改成同步，便于测
svc.kick();
```

---

## 8. 教程 E：要返回值怎么办

executor 的 `post` **只跑 `void()`**。取值常见三种：

```cpp
// 1) 直接用线程池 submit（推荐要结果时）
auto fut = pool.submit([] { return 40 + 2; });
int v = fut.get();

// 2) promise 自己塞进 post
std::promise<int> p;
auto f = p.get_future();
ex.post([&p] {
    try {
        p.set_value(42);
    } catch (...) {
        p.set_exception(std::current_exception());
    }
});
int x = f.get();

// 3) 对象亲和 + 取值 → signal_and_slots 的 invoke
```

---

## 9. 场景速查

| 我想… | 步骤 |
|------|------|
| 单测默认同步 | A：`inline_executor` |
| 业务 post 到线程池 | B：`adapters.h` + `make_executor(pool)` |
| 投递到 UI/worker 事件循环 | C：`make_executor(*loop)` |
| 成员可换执行后端 | D：`any_executor` |
| 要返回值 | E：`submit` / `promise` / `invoke` |
| 有界不堵调用方 | `try_post` |

---

## 10. concept 与工厂（简表）

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

自写类型只要满足 `post`（最好也有 `try_post`），就能塞进 `any_executor`。

| 工厂 | 返回 |
|------|------|
| `make_inline_executor()` | `inline_executor` |
| `make_executor(thread_pool&)` | `thread_pool_executor` |
| `make_executor(event_loop&)` | `event_loop_executor` |

---

## 相关文件

- 核心：`concurrency/executor/executor.h`
- 适配：`concurrency/executor/adapters.h`
- 可调用擦除：`concurrency/detail/any_invocable.h`
- 示例：`demo/concurrency/executor/demo.cpp`
- 测试：`tests/executor_test.cpp`
- 后端文档：[thread_pool.md](./thread_pool.md)、[signal_and_slots.md](./signal_and_slots.md)

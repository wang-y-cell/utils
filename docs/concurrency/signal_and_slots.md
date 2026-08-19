# Signal & Slots 使用教程（`utils::signal_and_slots`）

轻量 **Qt 风格** 信号/槽与事件循环，C++20，header-only。  
本文以**怎么用**为主：列出全部可行操作、标出异常/危险用法，并给出分场景教程。类成员级 API 表不是重点。

| 项 | 说明 |
|----|------|
| 头文件 | [`concurrency/signal_and_slots/signal_and_slots.h`](../../concurrency/signal_and_slots/signal_and_slots.h) |
| 命名空间 | `utils` |
| 依赖 | [`reliability/result/expected.h`](../../reliability/result/expected.h)（`result<T>`） |
| 可运行示例 | `demo/concurrency/signal_and_slots/sample.cpp`（目标：`demo_signal`） |

```bash
cmake --build build --target demo_signal
./build/demo_signal   # Windows: build\demo_signal.exe
```

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：同线程按钮与窗口](#4-教程-a同线程按钮与窗口)
5. [教程 B：连接类型怎么选](#5-教程-b连接类型怎么选)
6. [教程 C：后台 worker 跨线程](#6-教程-c后台-worker-跨线程)
7. [教程 D：取值 invoke](#7-教程-d取值-invoke)
8. [教程 E：断开、唯一连接、阻塞信号](#8-教程-e断开唯一连接阻塞信号)
9. [教程 F：堆对象与 delete_later](#9-教程-f堆对象与-delete_later)
10. [教程 G：主线程持续泵事件](#10-教程-g主线程持续泵事件)
11. [教程 H：定时器与临时排空](#11-教程-h定时器与临时排空)
12. [场景速查](#12-场景速查)
13. [与 Qt 对照](#13-与-qt-对照)

---

## 1. 五分钟心智模型

```text
object  ──亲和──►  utils::thread*   （身份：在哪个线程上“活着”）
                      │
                      └──loop──►  event_loop   （投递：任务队列 / 定时器）

signal.emit(...)
   ├─ 同线程 Auto/Direct  → 立刻调槽
   └─ 跨线程 Auto/Queued → post 到 receiver->loop()
```

三条硬规则：

1. **亲和用 `thread*`，不用 `event_loop*`。** 跨线程请 `move_to_thread(worker)`。
2. **日常不要直接 `new event_loop` 当亲和。** 主线程用 `core_application` / `ensure_thread()`；后台用 `worker_thread`。
3. **`connect` 不延长寿命。** 接收者死了连接就断；Queued 执行前还会查 `is_valid()` / lifetime。

推荐入口：

| 你想… | 用 |
|------|----|
| 写 UI / 业务对象 | 继承 `object`，成员 `signal{this}`，槽返回 `slots_t` |
| 主线程有默认 loop | `core_application app;` |
| 后台跑槽 | `worker_thread` + `move_to_thread` |
| 连起来 | `connect(sig, receiver, &Class::slot)` 或 lambda |
| 发事件 | `sig.emit(...)` 或 `sig(...)` |
| 同步取值 | `invoke(..., connection_type::blocking_queued, ...)` |

---

## 2. 全部可行操作一览

下面按「能做什么」列出；每项后的「见教程」指向详细步骤。

### 2.1 定义对象与信号

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 继承 `object` | `class W : public object { ... };` | 获得亲和、入站连接、lifetime |
| 析构时失效 | `~W() override { invalidate(); }` | **派生析构第一行** |
| 声明信号 | `signal<> clicked{this};` | 绑 `this` 才能吃到 `object::block_signals` |
| 带参信号 | `signal<int, std::string> changed{this};` | `emit` 按值入参 |
| 成员槽 | `slots_t<> onClick();` / `slots_t<int> value();` | 成员槽**必须**返回 `slots_t` |
| lambda 槽 | `connect(sig, &obj, [](...){...});` | 不要求 `slots_t`，但必须绑 `object*` |

> 见 [教程 A](#4-教程-a同线程按钮与窗口)。

### 2.2 连接与断开

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 连成员槽 | `connect(sig, &recv, &Class::slot);` | 默认 `automatic` |
| 指定类型 | `connect(..., connection_type::queued);` | 见 [教程 B](#5-教程-b连接类型怎么选) |
| 唯一连接 | `connect(..., unique_connection);` | 同接收者+同成员槽只连一次；**对 lambda 无效** |
| 连智能指针 | `connect(sig, uptr, &Class::slot);` | 只观察，不延长寿命 |
| 拿连接句柄 | `connection c = connect(...);` | 可手动 `c.disconnect()` |
| RAII 断开 | `scoped_connection sc{connect(...)};` | 离开作用域自动断 |
| 按接收者断 | `sig.disconnect(&recv);` | 该接收者在本信号上的全部连接 |
| 全断 | `sig.disconnect_all();` | 信号析构也会做 |

### 2.3 发射与阻塞

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 发射 | `sig.emit(args...);` / `sig(args...);` | 丢弃槽返回值 |
| 阻塞单个信号 | `sig.block_signals(true);` | 仅本信号 |
| 阻塞对象全部发出信号 | `obj.block_signals(true);` | 成员信号须 `signal{this}` |

### 2.4 线程亲和与后台线程

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 查当前线程句柄 | `ensure_thread()` | 仿 `QThread::currentThread()` |
| 查对象亲和 | `obj.thread()` | `utils::thread*`；句柄已毁为 `nullptr` |
| 查投递 loop | `obj.loop()` / `obj.loop_shared()` | worker `stop` 后可能为空 |
| 迁到 worker | `obj.move_to_thread(worker);` | 之后 Queued 在 worker 上跑 |
| 迁回当前线程 | `obj.move_to_thread(nullptr);` | 回到调用方的 `ensure_thread()` |
| 启动/停止 worker | `worker.start();` / `worker.stop();` | **必须在外线程 `stop`** |
| 主线程注册 | `core_application app;` | 构造即 `ensure_thread()` |
| 主线程泵事件 | `app.exec();` | 阻塞直到 `thread->stop()` |

> 见 [教程 C](#6-教程-c后台-worker-跨线程)、[教程 G](#10-教程-g主线程持续泵事件)。

### 2.5 invoke（在亲和线程执行并可选取值）

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 调成员槽取值 | `invoke(&obj, &Class::slot, args...);` | 默认 Auto；跨线程取值须 Blocking |
| 指定类型取值 | `invoke(&obj, &Class::slot, connection_type::blocking_queued, args...);` | 目标须**正在泵** |
| 调 lambda 取值 | `invoke(&obj, connection_type::blocking_queued, fn, args...);` | **必须带 type** |
| 只投递不取值 | `invoke(&obj, std::function<void()>{...});` | 跨线程默认 Queued，不阻塞 |

> 见 [教程 D](#7-教程-d取值-invoke)。

### 2.6 堆对象生命周期

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 堆上 unique | `auto p = make_object_unique<Window>(...);` | 释放走 `delete_later` |
| 堆上 shared | `auto p = make_object_shared<Window>(...);` | 删除器同上 |
| 预约删除 | `p->delete_later();` | 尽量在亲和线程串行销毁 |
| 取裸指针 | `object_get(p)` | 适配裸/unique/shared |

> 见 [教程 F](#9-教程-f堆对象与-delete_later)。

### 2.7 事件循环（一般通过 thread 间接用）

| 操作 | 推荐入口 | 说明 |
|------|----------|------|
| 投递任务 | `ensure_thread()->loop()->post(fn);` 或 Queued 连接 | 未 accepting 时丢弃 |
| 阻塞投递 | `loop->post_blocking(fn);` | 见 Tips：未泵/自泵会失败 |
| 延迟/周期 | `post_delayed` / `post_periodic` | 可用 `cancel_timer` |
| 排空一点 | `loop->process_events(budget);` | 测试/短等待；不适合当长期 worker |
| 长期泵 | `worker.start()` 或 `app.exec()` | 不要手写裸 `run` 当业务主路径 |

> 见 [教程 H](#11-教程-h定时器与临时排空)。

---

## 3. 异常操作与 Tips（必读）

### 3.1 禁止 / 必挂的操作

| 异常操作 | 后果 | 正确做法 |
|----------|------|----------|
| 在 **worker 自己的线程** 里 `worker.stop()` | assert 硬失败 | 只在创建它的线程（通常主线程）`stop` |
| `move_to_thread(某个 event_loop*)` | **没有该重载**；亲和不是 loop | `move_to_thread(worker)` / `thread*` |
| 成员槽返回 `void` / `int` | **无法** `connect` 成员槽 | 返回 `slots_t<>` / `slots_t<T>` |
| 派生析构不先 `invalidate()` | Queued 槽可能摸到已毁成员 | 析构**第一行** `invalidate()` |
| 指望 `connect` 延长 `shared_ptr` 寿命 | 对象提前销毁，连接变空 | 自己管寿命；或 `object_uptr` |
| 对**未在泵**的目标做 BlockingQueued / `post_blocking` | 立即失败或跳过（不再死等） | 目标先 `start`/`exec`，或改用 Queued |
| 在正在泵**本** loop 的线程里对本 loop `post_blocking` | 返回 `false`（防自死锁） | 同线程用 Direct / 直接调用 |
| worker `stop` 后仍以为 Auto 会在发射线程执行槽 | **不会**；无 loop 时跨线程连接被跳过 | `start` 后再 emit，或先 `move_to_thread(nullptr)` |

### 3.2 容易误解的行为（Tips）

**Tip 1 — `is_running` ≠ `is_pumping`**

- `is_running()`：还接不接受 `post`（accepting）。
- `is_pumping()`：有没有线程正在 `run` / `process_events`。
- BlockingQueued 看的是 **`is_pumping()`**。主线程只构造了 `core_application`、没 `exec` 时，从 worker Blocking 回主线程会失败/跳过，而不是挂死。

**Tip 2 — worker `stop` 再 `start`**

- 亲和仍是同一个 `worker_thread*`。
- `stop` 后 `loop()` 为空 → Queued/Auto/Direct（跨线程）**安全跳过**。
- 再 `start` 后**不必重新 `move_to_thread`**，新 loop 自动接上。

**Tip 3 — 跨线程 Direct**

- 会**降级为 Queued**（有 loop 时）。
- 无 loop 时：**跳过**，不会在发射线程硬调（避免错线程）。

**Tip 4 — `unique_connection` 只管成员槽**

- lambda 的 `unique` 参数被忽略；连两次 lambda 就是两条连接。

**Tip 5 — move-only 参数**

- `emit` 多个 Queued 连接时，move-only 参数只有第一次 move 有效。多连接请用可拷贝参数。

**Tip 6 — Queued 槽抛异常**

- 事件循环会吞掉单任务异常，后续任务继续。不要靠异常做跨线程控制流。

**Tip 7 — `invoke` 跨线程取值**

- 只有 `blocking_queued` 能同步拿到 `result`。
- Auto/Queued/跨线程 Direct → `operation_in_progress` 一类错误，不是“投递成功”。

**Tip 8 — 不要把裸 `event_loop` 当应用架构**

- 测试、`process_events`、适配器可以碰 loop。
- 业务对象亲和请走 `object` + `thread` / `worker_thread` / `core_application`。

---

## 4. 教程 A：同线程按钮与窗口

目标：主线程上定义信号、连接成员槽与 lambda、发射。

```cpp
#include "concurrency/signal_and_slots/signal_and_slots.h"
#include <iostream>
#include <string>

using namespace utils;

class Button : public object {
public:
    signal<> clicked{this};
    signal<std::string> textChanged{this};
};

class Window : public object {
public:
    ~Window() override { invalidate(); }   // 必须第一行

    slots_t<> onClicked() {
        std::cout << "clicked\n";
        return {};
    }

    slots_t<> onText(const std::string& s) {
        std::cout << "text=" << s << "\n";
        return {};
    }
};

int main() {
    core_application app;   // 注册主线程 thread + 默认 loop

    Button btn;
    Window win;

    connect(btn.clicked, &win, &Window::onClicked);
    connect(btn.textChanged, &win, &Window::onText);
    connect(btn.clicked, &win, [] {
        std::cout << "lambda also saw click\n";
    });

    btn.clicked.emit();
    btn.textChanged.emit("hello");
    return 0;
}
```

步骤说明：

1. `core_application`：之后在本线程创建的 `object` 默认亲和到主线程句柄。
2. `signal{this}`：发送者 `block_signals` 才能挡住这个信号。
3. 同线程默认 Auto → Direct，槽在 `emit` 调用栈里同步执行。
4. 不需要 `app.exec()` 也能跑同线程 Direct；只有 Queued/定时器进主线程队列时才需要泵。

---

## 5. 教程 B：连接类型怎么选

```cpp
enum class connection_type {
    direct,            // 同步；跨线程有 loop 时降为 Queued
    queued,            // 异步投递到 receiver->loop()
    blocking_queued,   // 投递并等待（目标须 is_pumping）
    automatic          // 同线程 Direct；跨线程 Queued
};
```

### 怎么选

| 场景 | 推荐 |
|------|------|
| 同线程 UI 刷新、简单回调 | `automatic`（默认）或 `direct` |
| 通知后台“有活”，不要等结果 | `queued` 或跨线程 `automatic` |
| 必须等对方线程算完再继续 | `blocking_queued` |
| 跨线程还要返回值 | `invoke` + `blocking_queued` |

### 决策树

```text
需要返回值？
  ├─ 是 → invoke + blocking_queued（目标必须正在泵）
  └─ 否
        ├─ 同线程 → automatic / direct
        └─ 跨线程
              ├─ 要等完成 → blocking_queued
              └─ 不等 → queued / automatic
```

### 示例：同对象两种连接

```cpp
connect(btn.clicked, &ui, &Window::onClicked);                    // Auto
connect(btn.clicked, &worker_win, &Window::onClicked,
        connection_type::queued);                               // 强制异步
connect(btn.clicked, &worker_win, &Window::onClicked,
        connection_type::blocking_queued);                      // 等 worker 跑完
```

**Tip：** Blocking 前请确认目标已在泵，例如：

```cpp
ASSERT(wait_until([&] {
    auto* loop = worker.loop();
    return loop && loop->is_pumping();
}));
```

只等 `worker.is_running()` 不够——loop 可能已 accepting 但尚未进入 `run()`。

---

## 6. 教程 C：后台 worker 跨线程

目标：窗口迁到 worker，主线程 emit，槽在后台执行。

```cpp
using namespace std::chrono_literals;

int main() {
    core_application app;

    worker_thread worker;
    worker.start();

    Button btn;                 // 仍在主线程
    Window win;
    win.move_to_thread(worker); // 亲和 → worker

    connect(btn.clicked, &win, &Window::onClicked); // Auto → 跨线程 Queued

    btn.clicked.emit();
    std::this_thread::sleep_for(50ms);  // 演示用；生产可改信号/条件变量

    worker.stop();   // 必须在主线程（外线程）调用
    return 0;
}
```

### stop / start 再发射

```cpp
worker.stop();
btn.clicked.emit();   // win.loop()==nullptr → 安全跳过，不 UAF
worker.start();
btn.clicked.emit();   // 自动进新 loop，无需再 move_to_thread

win.move_to_thread(nullptr);  // 迁回主线程 ensure_thread()
```

### 完整推荐顺序

1. `core_application app;`
2. `worker.start();` 并等到 `loop()->is_pumping()`（若要用 Blocking）
3. `obj.move_to_thread(worker);`
4. `connect` / `emit` / `invoke`
5. 销毁对象或 `move_to_thread(nullptr)` / `delete_later`
6. **最后** `worker.stop();`

**异常：** 在槽函数里 `worker.stop()` → 禁止。

---

## 7. 教程 D：取值 invoke

`emit` / 普通 `connect` **丢掉**返回值。要取值用 `invoke`。

### 成员槽取值

```cpp
class Window : public object {
public:
    ~Window() override { invalidate(); }
    slots_t<int> nameLen() { return static_cast<int>(name_.size()); }
private:
    std::string name_ = "MainWindow";
};

Window win;
auto r = invoke(&win, &Window::nameLen);  // 同线程 Auto → Direct
if (r) std::cout << *r << "\n";
```

### 跨线程取值（必须 Blocking + 目标在泵）

```cpp
worker_thread worker;
worker.start();
// 等到 is_pumping() ...

Window win;
win.move_to_thread(worker);

auto len = invoke(&win, &Window::nameLen,
                  connection_type::blocking_queued);
if (len) {
    std::cout << *len << "\n";
} else {
    std::cout << "err: " << len.error().message() << "\n";
}
```

### lambda 取值（必须带 connection_type）

```cpp
auto sum = invoke(&win, connection_type::blocking_queued,
                  [](int a, int b) { return a + b; }, 40, 2);
```

### 只投递、不要返回值

```cpp
invoke(&win, [&] {
    std::cout << "runs on win's thread\n";
}); // 跨线程默认 Queued，主线程不阻塞
```

### 常见错误码

| `std::errc` | 何时 |
|-------------|------|
| `invalid_argument` | 空指针等 |
| `operation_in_progress` | 跨线程却用了无法同步取值的类型 |
| `operation_not_permitted` | 无 loop、目标未泵、`post_blocking` 失败 |
| `owner_dead` | 对象已 `invalidate` |

---

## 8. 教程 E：断开、唯一连接、阻塞信号

### 唯一连接

```cpp
connect(btn.clicked, &win, &Window::onClicked,
        connection_type::automatic, unique_connection);

auto c2 = connect(btn.clicked, &win, &Window::onClicked,
                  connection_type::automatic, unique_connection);
assert(!c2.connected());  // 第二次失败
```

### scoped_connection

```cpp
{
    scoped_connection sc{connect(btn.clicked, &win, &Window::onClicked)};
    btn.clicked.emit();   // 触发
}                         // 自动断开
btn.clicked.emit();       // 不再触发
```

### 按接收者断开 / 阻塞

```cpp
btn.clicked.disconnect(&win);

btn.block_signals(true);
btn.clicked.emit();       // 全部被挡（信号绑了 this）
btn.block_signals(false);

btn.clicked.block_signals(true);  // 只挡这一个信号
```

---

## 9. 教程 F：堆对象与 delete_later

跨线程时优先堆对象 + `delete_later`，让销毁与 Queued 槽串行。

```cpp
auto win = make_object_unique<Window>("Main");
connect(btn.clicked, win, &Window::onClicked);

win.reset();  // → delete_later → 尽量在亲和线程 delete
```

规则摘要：

- 无 loop：立即 `delete`。
- 不在亲和线程，或本线程正在泵亲和 loop：`post` 删除任务；post 失败则立即 `delete`（防泄漏）。
- 亲和线程且未在泵：立即 `delete`。

**Tip：** 即使用 `object_uptr`，派生类仍建议析构第一行 `invalidate()`。

---

## 10. 教程 G：主线程持续泵事件

同线程 Direct 不需要 `exec`。若主线程要收 Queued / 定时器 / `delete_later` 投递，需要泵：

```cpp
int main() {
    core_application app;

    // 安排工作…
    ensure_thread()->loop()->post([] {
        // 在主线程队列里跑
    });

    // 另一线程稍后 stop 主线程循环：
    // std::thread([&]{ ...; app.thread()->stop(); }).detach();

    return app.exec();  // 阻塞泵，直到 stop
}
```

或短时间排空（测试常用）：

```cpp
app.loop()->process_events(std::chrono::milliseconds(100));
```

**Tip：** 从 worker BlockingQueued 回主线程时，主线程必须正在 `exec`/`process_events`（`is_pumping()`），否则调用失败/跳过。

---

## 11. 教程 H：定时器与临时排空

一般通过当前线程 loop（不要自建亲和用 loop）：

```cpp
using namespace std::chrono_literals;

event_loop* loop = ensure_thread()->loop();

loop->post([] { std::cout << "asap\n"; });

auto id = loop->post_periodic(50ms, [] {
    std::cout << "tick\n";
});

loop->process_events(200ms);
loop->cancel_timer(id);
```

后台长期定时：把对象 `move_to_thread(worker)`，在槽里逻辑处理；或由 worker 的 loop 投递（worker `start` 后 `worker.loop()->post_periodic(...)`）。

**Tip：** `process_events` 适合测试与短暂排空；长期运行用 `worker_thread::start` / `app.exec`。

---

## 12. 场景速查

| 我想… | 步骤 |
|------|------|
| 最小同线程信号槽 | A：`core_application` + `connect` + `emit` |
| 后台处理点击 | C：`worker.start` → `move_to_thread` → Auto/Queued |
| 等后台算完再继续 | B/D：`blocking_queued` 或 `invoke(..., blocking_queued)` |
| 临时监听 | E：`scoped_connection` |
| 批量改状态不发信号 | E：`obj.block_signals(true)` |
| 堆上窗口跨线程销毁 | F：`make_object_unique` |
| 主线程收异步任务 | G：`app.exec()` 或 `process_events` |
| 周期性刷新 | H：`post_periodic`（目标线程在泵） |
| worker 重启 | C：`stop` →（emit 安全跳过）→ `start` → 再 emit |

---

## 13. 与 Qt 对照

| Qt | 本库 |
|----|------|
| `QObject` | `object` |
| `signals:` / 宏 | `signal<Args...>` 成员 |
| 槽 `void` / 任意返回 | 成员槽须 `slots_t`；lambda 不限 |
| `QObject::connect` | `connect` / `signal::connect` |
| `Qt::ConnectionType` | `connection_type` |
| `QMetaObject::invokeMethod` | `invoke`（`result` 取值） |
| `QCoreApplication` | `core_application` |
| `QThread` / `currentThread()` | `utils::thread` / `ensure_thread()` |
| 工作线程 + 事件循环 | `worker_thread` |
| `QObject::thread()` | `object::thread()` → `thread*` |
| `deleteLater` | `delete_later` / `object_uptr` |
| `blockSignals` | `block_signals`（信号需 `signal{this}`） |

---

## 相关文件

- 实现：`concurrency/signal_and_slots/signal_and_slots.h`
- 示例：`demo/concurrency/signal_and_slots/sample.cpp`
- 测试：`tests/signal_and_slots_test.cpp`
- 错误类型：`reliability/result/expected.h`（`result` / `result_ok` / `result_err`）

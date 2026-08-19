# Signal & Slots（`utils::signal_and_slots`）

轻量 **Qt 风格** 信号/槽与事件循环库，C++20，header-only。

| 项 | 说明 |
|----|------|
| 头文件 | [`concurrency/signal_and_slots/signal_and_slots.h`](../concurrency/signal_and_slots/signal_and_slots.h) |
| 命名空间 | `utils` |
| 依赖 | [`reliability/result/expected.h`](../reliability/result/expected.h)（`result<T>`） |
| 可运行示例 | `demo/concurrency/signal_and_slots/sample.cpp`（目标：`demo_signal`） |

```bash
cmake --build build --target demo_signal
./build/demo_signal   # Windows: build\demo_signal.exe
```

---

## 目录

1. [快速入门](#1-快速入门)
2. [核心概念](#2-核心概念)
3. [connection_type](#3-connection_type)
4. [slots_t](#4-slots_t)
5. [object](#5-object)
6. [signal](#6-signal)
7. [connection / scoped_connection](#7-connection--scoped_connection)
8. [connect（自由函数）](#8-connect自由函数)
9. [invoke](#9-invoke)
10. [event_loop](#10-event_loop)
11. [thread / ensure_thread](#11-thread--ensure_thread)
12. [core_application](#12-core_application)
13. [worker_thread](#13-worker_thread)
14. [智能指针与 delete_later](#14-智能指针与-delete_later)
15. [线程与生命周期注意事项](#15-线程与生命周期注意事项)
16. [与 Qt 对照](#16-与-qt-对照)

---

## 1. 快速入门

```cpp
#include "concurrency/signal_and_slots/signal_and_slots.h"
#include <iostream>
#include <string>

using namespace utils;

class Button : public object {
public:
    signal<> clicked{this};                 // 无参信号，绑定发送者以支持 block_signals
    signal<std::string> textChanged{this};
};

class Window : public object {
public:
    ~Window() override { invalidate(); }    // 派生析构第一行

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
    core_application app;   // 注册主线程默认 event_loop

    Button btn;
    Window win;

    connect(btn.clicked, &win, &Window::onClicked);
    connect(btn.textChanged, &win, &Window::onText);
    // 或 lambda：
    // connect(btn.clicked, &win, [] { std::cout << "lambda\n"; });

    btn.clicked.emit();
    btn.textChanged.emit("hello");
    return 0;
}
```

要点：

- 接收者继承 `object`。
- **成员槽** 必须返回 `slots_t<>` / `slots_t<T>`。
- **lambda 槽** 不要求 `slots_t`，但必须绑定到某个 `object*`（用于线程亲和与寿命）。
- 成员信号建议写成 `signal{this}`，以便 `object::block_signals` 生效。

---

## 2. 核心概念

### 2.1 信号与槽

- **信号（`signal<Args...>`）**：可 `emit` 的事件源；可连接多个槽。
- **槽**：对信号的响应。两种形式：
  - 成员函数：`slots_t<R> Class::method(Args...)`
  - 可调用对象：`connect(sig, receiver, lambda)`

`emit` / `connect` **丢弃**槽返回值；只有 [`invoke`](#9-invoke) 会取回值。

### 2.2 线程亲和（Thread Affinity）

每个 `object` 绑定一个 `utils::thread*`（仿 `QObject`）：

- 构造时绑到 `ensure_thread()`（当前 OS 线程句柄，TLS，不新开线程）。
- 跨线程用 `move_to_thread(worker)`（`worker_thread` 继承 `thread`）。
- `move_to_thread(nullptr)` 回到 `ensure_thread()`（调用方当前线程）。
- `object::thread()` 返回亲和句柄；`object::loop()` 解析投递用的 `event_loop*`。
- `Queued` / 跨线程 `Auto` 投递到 `loop()`；无 loop 则丢弃。

**Tip：** 绑 worker 后 `stop()` → `loop()` 为 `nullptr`，Queued emit 安全跳过；再 `start()` 无需重新 `move`。

### 2.3 连接不延长寿命

`connect` **只观察**接收者，不会因连接而延长 `shared_ptr` 寿命。对象销毁后入站连接会断开；Queued 槽执行前还会检查 `is_valid()`。

---

## 3. connection_type

```cpp
enum class connection_type {
    direct,            // 在 emit 所在线程同步执行
    queued,            // 投递到接收者 event_loop，异步
    blocking_queued,   // 投递并等待目标线程执行完毕
    automatic          // 同线程 → Direct；跨线程 → Queued
};
```

| 类型 | 行为 | 典型场景 |
|------|------|----------|
| `direct` | 同步调用。跨线程时**降级为 Queued** | 同线程 UI 刷新 |
| `queued` | 异步投递；无目标 loop 则丢弃 | 跨线程通知 |
| `blocking_queued` | 跨线程等待完成；同线程当 Direct。目标 loop 须正在 `run` | 跨线程 `invoke` 取值 |
| `automatic` | 默认；按是否同线程自动选择 | 大多数连接 |

常量：

```cpp
inline constexpr bool unique_connection = true;
```

用于 `connect(..., unique_connection)`，表示同一接收者 + 同一成员槽只连一次。

---

## 4. slots_t

成员槽的**返回类型标记**（仿 Qt 的「槽」约定，编译期约束）。

### 4.1 声明

```cpp
template <class T = void>
class slots_t;

// void 特化
slots_t<>          // 无业务返回值
slots_t<int>       // 带一个 int，供 invoke 取出
```

### 4.2 API

| 成员 | 说明 |
|------|------|
| `slots_t()` | 默认构造 |
| `slots_t(U&&)` | 由可构造为 `T` 的值构造（非 void） |
| `T& get() &` | 取回内部值 |
| `const T& get() const&` | |
| `T get() &&` | 移动取出 |
| `operator T() const noexcept` | 隐式类型转换 |

### 4.3 示例

```cpp
class Counter : public object {
public:
    slots_t<> bump() {
        ++n_;
        return {};
    }

    slots_t<int> value() {
        return n_;   // 隐式构造成 slots_t<int>
    }

private:
    int n_ = 0;
};

// connect / emit 丢弃返回值
connect(sig, &c, &Counter::bump);

// 只有 invoke 取 get()
auto r = invoke(&c, &Counter::value);  // result<int>
if (r) std::cout << *r;
```

> **注意**：普通 `void` / `int` 成员函数**不能**作为成员槽 `connect`；请返回 `slots_t`。lambda 不受此限。

---

## 5. object

仿 `QObject`：线程亲和、入站连接追踪、存活令牌、`delete_later`。

### 5.1 构造 / 析构

```cpp
object();
object(const object&) = delete;
virtual ~object();   // 内部调用 invalidate()
```

构造时自动：`_affinity = ensure_thread()`。

### 5.2 生命周期

| API | 说明 |
|-----|------|
| `void invalidate() noexcept` | 标记失效、重置 lifetime、断开全部入站连接；若当前在亲和线程则 `process_events()` 排空已排队任务 |
| `bool is_valid() const` | 是否仍有效 |
| `std::weak_ptr<void> lifetime() const` | 存活弱引用，供槽内校验 |

**派生类析构第一行必须调用 `invalidate()`**（在成员销毁之前）：

```cpp
class Window : public object {
public:
    ~Window() override {
        invalidate();   // 必须第一行
        // 然后才销毁成员…
    }
};
```

原因：C++ 先跑 `~Derived` 体与成员析构，再进 `~object`。若不提前 `invalidate()`，Queued 槽可能在派生成员已毁后仍认为对象“活着”。

堆对象优先使用 [`object_uptr` / `delete_later`](#13-智能指针与-delete_later)，可降低漏写概率，但**最稳妥仍建议写 `invalidate()`**。

### 5.3 线程亲和

| API | 说明 |
|-----|------|
| `void move_to_thread(thread*)` | 绑定线程句柄；`nullptr` → `ensure_thread()` |
| `void move_to_thread(thread&)` | 同上（含 `worker_thread&`） |
| `thread* thread() const` | 亲和句柄（仿 `QObject::thread`）；句柄已毁为 `nullptr` |
| `std::shared_ptr<event_loop> loop_shared() const` | 投递目标的共享所有权；内部 `emit`/`invoke`/`delete_later` 持有它，避免与 `worker.stop()` 并发 UAF |
| `event_loop* loop() const` | `loop_shared().get()`；worker 已 stop 时可能为 `nullptr` |
| `worker_thread* worker() const` | `dynamic_cast`；非 worker 亲和时为 `nullptr` |

```cpp
worker_thread worker;
worker.start();

Window win;
EXPECT(win.thread() == ensure_thread());
win.move_to_thread(worker);
// 之后 Queued 槽在 worker 线程执行

worker.stop();
sig.emit(...);                // loop()==nullptr → Queued 安全跳过
worker.start();
sig.emit(...);                // 自动进新 loop，无需再 move

win.move_to_thread(nullptr);  // 回到 ensure_thread()
```

### 5.4 删除与阻塞信号

| API | 说明 |
|-----|------|
| `void delete_later()` | 在亲和线程上 `delete this`（必要时 `post`） |
| `void block_signals(bool)` | 暂停/恢复**本对象作为发送者**发出的信号（成员信号需 `signal{this}`） |
| `bool signals_blocked() const` | 是否被阻塞 |

```cpp
btn.block_signals(true);
btn.clicked.emit();   // 不会触发槽
btn.block_signals(false);
```

### 5.5 入站连接（一般由库内部使用）

| API | 说明 |
|-----|------|
| `void track_inbound(...)` | 登记入站连接 |
| `void untrack_inbound(id)` | 移除 |

---

## 6. signal

```cpp
template <typename... Args>
class signal;
```

### 6.1 构造

```cpp
signal<>();                          // 无 owner
signal<> clicked{this};              // 推荐：绑定发送者 object*
```

不可拷贝。析构时 `disconnect_all()`。

### 6.2 阻塞

| API | 说明 |
|-----|------|
| `void block_signals(bool)` | 阻塞本信号 |
| `bool signals_blocked() const` | 本信号或 owner 的 `block_signals` |

### 6.3 连接

**成员槽**（必须返回 `slots_t`）：

```cpp
connection connect(Recv* receiver,
                   slots_t<R> (Class::*method)(Args...),
                   connection_type type = automatic,
                   bool unique = false);

// const 成员槽、unique_ptr / shared_ptr / weak_ptr 重载同理
```

**lambda / 可调用对象**（不要求 `slots_t`）：

```cpp
connection connect(Recv* receiver, F&& func,
                   connection_type type = automatic,
                   bool unique = false);  // unique 对 lambda 无效（忽略）
```

执行前会检查 `receiver->is_valid()`。

### 6.4 断开

| API | 说明 |
|-----|------|
| `void disconnect(connection&)` | 断开一条连接 |
| `void disconnect(object* receiver)` | 断开该接收者在本信号上的全部连接 |
| `void disconnect_all()` | 断开全部 |

智能指针接收者有对应 `disconnect` 重载。

### 6.5 发射

```cpp
void emit(Args... args) const;   // 按值入参（decay-copy）
void operator()(Args... args) const;  // 同 emit
```

- 支持同线程重入 `emit`（快照 + thread_local 缓冲）。
- Queued / Blocking 路径会再打包参数；执行前校验 lifetime / `is_valid()`。
- move-only 参数：多个 Queued 连接时仅第一次 move 有效，建议单连接或可拷贝包装。

### 6.6 完整示例

```cpp
class Button : public object {
public:
    signal<> clicked{this};
    signal<int> valueChanged{this};
};

class Display : public object {
public:
    ~Display() override { invalidate(); }

    slots_t<> refresh() {
        std::cout << "refresh\n";
        return {};
    }

    slots_t<> onValue(int v) {
        std::cout << "v=" << v << "\n";
        return {};
    }
};

Button btn;
Display disp;

btn.clicked.connect(&disp, &Display::refresh);
btn.valueChanged.connect(&disp, [](int v) {
    std::cout << "lambda " << v << "\n";
});

btn.clicked.emit();
btn.valueChanged.emit(42);

btn.valueChanged.disconnect(&disp);
```

---

## 7. connection / scoped_connection

### 7.1 connection

表示一条可手动断开的连接句柄（内部 `shared_ptr` 状态）。

| API | 说明 |
|-----|------|
| `bool connected() const` | 是否仍连接 |
| `void disconnect()` | 断开 |
| `std::uint64_t id() const` | 连接 id |

```cpp
connection c = connect(btn.clicked, &win, &Window::onClicked);
// ...
c.disconnect();
```

### 7.2 scoped_connection

RAII：析构或重新赋值时自动 `disconnect`。

| API | 说明 |
|-----|------|
| `scoped_connection(connection)` | |
| `void disconnect()` | 提前断开 |
| `bool connected() const` | |
| `connection release()` | 交出所有权，不再自动断 |

不可拷贝，可移动。

```cpp
{
    scoped_connection sc{connect(btn.clicked, &win, &Window::onClicked)};
    btn.clicked.emit();  // 会触发
}                        // 离开作用域自动断开
btn.clicked.emit();      // 不再触发
```

---

## 8. connect（自由函数）

语法糖，等价于 `signal_.connect(...)`。

```cpp
connection connect(signal<Args...>& sig, Recv* receiver,
                   slots_t<R> (Class::*method)(...),
                   connection_type type = automatic,
                   bool unique = false);

connection connect(signal<Args...>& sig, Recv* receiver, F&& func,
                   connection_type type = automatic,
                   bool unique = false);
```

支持 `unique_ptr` / `shared_ptr` / `weak_ptr` 接收者。

```cpp
connect(btn.clicked, &win, &Window::onClicked,
        connection_type::automatic, unique_connection);

// 第二次相同成员槽连接失败（返回未连接的 connection）
auto c2 = connect(btn.clicked, &win, &Window::onClicked,
                  connection_type::automatic, unique_connection);
assert(!c2.connected());
```

---

## 9. invoke

在目标 `object` 所在线程执行调用。分三类。

### 9.1 成员槽 → `result<T>`

取出 `slots_t<T>` 中的值。

```cpp
result<R> invoke(Recv* receiver, slots_t<R> (C::*method)(Args...),
                 connection_type type, Args... args);

result<R> invoke(Recv* receiver, slots_t<R> (C::*method)(Args...),
                 Args... args);  // type = automatic
```

```cpp
auto len = invoke(&win, &Window::nameLen,
                  connection_type::blocking_queued);
if (len) {
    std::cout << *len << "\n";
} else {
    std::cout << len.error().message() << "\n";
}
```

### 9.2 普通函数 / lambda → `result<R>`

**必须带 `connection_type`**，避免与「只投递 void」重载冲突。

```cpp
auto invoke(object* receiver, connection_type type, F&& fn, Args&&... args)
    -> result<invoke_result_t>;
```

```cpp
int add(int a, int b) { return a + b; }

auto sum = invoke(&win, connection_type::blocking_queued, add, 40, 2);
// *sum == 42

auto r = invoke(&win, connection_type::blocking_queued,
                [](int x) { return x * 2; }, 21);
```

无返回值时得到 `result<void>`：跨线程 + `blocking_queued` **仍会阻塞**等到执行完。

### 9.3 只投递、不要返回值 → `void`

```cpp
void invoke(object* receiver, std::function<void()> fn,
            connection_type type = automatic);
```

```cpp
invoke(&win, [&] {
    std::cout << "on win's thread\n";
});
// 跨线程默认 automatic → Queued 投递，不阻塞
```

显式 `blocking_queued` 时会阻塞等待。

### 9.4 跨线程如何拿到返回值

只能使用 **`connection_type::blocking_queued`**，且目标 loop 正在 `run`：

```cpp
worker_thread worker;
worker.start();
win.move_to_thread(worker);

auto v = invoke(&win, connection_type::blocking_queued, &Window::nameLen);
```

| type（跨线程） | 取值版行为 |
|----------------|------------|
| `blocking_queued` | 阻塞等待，返回 `result` |
| `automatic` / `queued` / `direct` | 无法同步取值 → `operation_in_progress` 等错误 |

常见错误码：

| `std::errc` | 含义 |
|-------------|------|
| `invalid_argument` | 空指针等 |
| `operation_in_progress` | 无法同步取值（Queued / 跨线程非 Blocking） |
| `operation_not_permitted` | loop 未 run，或 `post_blocking` 失败（含自死锁拒绝） |
| `owner_dead` | 对象已 `invalidate` |

---

## 10. event_loop

> **⚠️ 不推荐直接使用 `event_loop`**
>
> 直接使用时需要自行管理 `run()` / `stop()` 与生命周期。
> **`object` 不能 `move_to_thread(event_loop*)`**；跨线程请用 `worker_thread`。
>
> **请优先使用以下三种封装方式：**
>
> | 场景 | 推荐方式 |
> |------|----------|
> | 应用程序主线程事件循环 | [`core_application`](#12-core_application) / [`ensure_thread`](#11-thread--ensure_thread) |
> | 继承 `object` 在当前线程接收槽 | 继承 `object`，直接 `connect`（自动亲和到 `ensure_thread()`） |
> | 后台独立事件循环线程 | [`worker_thread`](#13-worker_thread) + `move_to_thread(worker)` |
>
> `event_loop` 本身保留给框架内部、测试、定时器/`post`，以及 `event_loop_executor` 等适配器。

每线程事件循环：任务队列 + 定时器。

### 10.1 类型别名

```cpp
using clock = std::chrono::steady_clock;
using task = std::function<void()>;
using timer_id = std::uint64_t;
```

### 10.2 API

| API | 说明 |
|-----|------|
| `bool post(task)` | 入队；loop 未 running 时返回 `false` 并丢弃 |
| `bool post_blocking(task)` | 入队并阻塞等待完成。**当前线程正在泵本 loop** → `false`（防自死锁）。**目标未在泵（`!is_pumping()`）** → `false`（避免对空闲主线程死等）。泵在任务开始前结束 → 取消该任务（不会稍后执行）并返回 `false`。其他线程对正在泵的 loop 阻塞投递合法（BlockingQueued） |
| `timer_id post_delayed(duration, task)` | 延迟执行；`delay<=0` 等价 `post`，返回 id `0` |
| `timer_id post_periodic(duration, task)` | 周期执行（首次在 interval 之后） |
| `void cancel_timer(timer_id)` | 取消定时器 |
| `void run()` | 阻塞泵送，直到 `stop()`；**不会**在入口把 accepting 设回 true（`stop()` 之后需 `set_accepting(true)` 才能再 `run`）。单任务异常被捕获。等待定时器时若插入更早的定时器会立即醒来 |
| `void stop()` | 结束 `run` |
| `bool is_running() const` | 是否仍接受 `post`（不等于正在泵） |
| `bool is_pumping() const` | 是否有人正在 `run`/`process_events` 驱动本 loop |
| `bool is_pumping_on_current_thread() const` | **当前 OS 线程**是否正在泵本 loop |
| `void process_events(optional<duration> budget = nullopt)` | 处理到期定时器与已排队任务；单任务异常同样被捕获 |

### 10.3 获取当前线程的 loop

不要再用独立的 loop TLS。当前线程的投递目标一律：

```cpp
ensure_thread()->loop();
```

### 10.4 示例

```cpp
using namespace std::chrono_literals;

event_loop* loop = ensure_thread()->loop();

loop->post([] { std::cout << "task\n"; });

auto id = loop->post_periodic(50ms, [] {
    std::cout << "tick\n";
});

// 非阻塞排空一点
loop->process_events(100ms);
loop->cancel_timer(id);
```

工作线程上通常由 `worker_thread::start()` 泵送，不必手写 `run()`。

> **`process_events()` 的适用场景**：测试、同步等待一批任务完成、或在没有完整事件循环的情况下手动排空队列。不适合长期运行的工作线程。

---

## 11. thread / ensure_thread

> **线程亲和句柄**（仿 `QThread` / `QThread::currentThread()`）。  
> `object` 只持有 `thread*`；`event_loop` 仅作投递实现。  
> **不要用 `event_loop*` 做亲和。**

| 类型 / API | 说明 |
|------------|------|
| `thread` | 抽象基类：`start()` / `stop()` / `loop_shared()` / `loop()` / `is_running()` / `identity()` |
| `current_thread` | 当前 OS 线程句柄（不 spawn），**拥有**本线程 `event_loop`；`start()` 在本线程阻塞泵 |
| `worker_thread` | 继承 `thread`；`start()` 创建新线程并泵 loop |
| `thread* ensure_thread()` | 当前线程句柄；worker 线程内指向该 `worker_thread` |

```cpp
thread* t = ensure_thread();          // 主线程：TLS current_thread
event_loop* loop = t->loop();         // current_thread 拥有的默认 loop
// t->start();                        // 本线程阻塞泵，直到 t->stop()

worker_thread worker;
worker.start();
// worker 线程内 ensure_thread() == &worker
```

注意与 `std::thread` 区分：本类型在命名空间 `utils` 中。

---

## 12. core_application

> **主线程应用程序的标准起点。**
> 构造后主线程所有 `object` 自动亲和到 `ensure_thread()`。

仿 `QCoreApplication`：注册当前线程句柄与默认 `event_loop`。

```cpp
class core_application {
public:
    core_application();           // ensure_thread()
    thread* thread() const;
    event_loop* loop() const;
    int exec();                   // thread->start()，返回 0
};
```

```cpp
int main() {
    core_application app;
    // 此后本线程创建的 object 默认亲和到 app.thread()
    // app.exec();  // 若需要主线程持续泵事件
    return 0;
}
```

即使不 `exec()`，只要构造过 `core_application`（或任意 `object` 触发了 `ensure_thread`），主线程也有默认亲和，可供 Direct / 同线程逻辑使用。跨线程 Queued 仍需要目标侧有人 `start`/`process_events`。

---

## 13. worker_thread

> **后台线程的标准方式。**  
> 继承 `thread`，**override** `start`/`stop`：创建 `event_loop`、在新线程 `run()`、将该 worker 注册为该 OS 线程的 `ensure_thread()`。

| API | 说明 |
|-----|------|
| `void start() override` | 创建 loop 并在**新线程** `run()` |
| `void stop() override` | `stop` loop 并 `join`。**禁止在工作线程内调用**（assert 硬失败） |
| `std::shared_ptr<event_loop> loop_shared() const override` | 未 start / 已 stop 时为空 |
| `event_loop* loop() const` | `loop_shared().get()` |
| `bool is_running() const override` | |
| `identity()` | 继承自 `thread` |

```cpp
worker_thread worker;
worker.start();

Window win;
win.move_to_thread(worker);

connect(btn.clicked, &win, &Window::onClicked);  // Auto → 跨线程 Queued
btn.clicked.emit();

worker.stop();    // 务必在外线程 stop；此后 Queued emit 安全跳过
worker.start();   // 再 start 后，已 move 过的 object 自动用新 loop
btn.clicked.emit();
worker.stop();
```

**Tip：** `object` 亲和持有的是 `thread*`（常为 `worker_thread*`），不是某次 `start` 的裸 `event_loop*`。同线程判断比较 `object::thread()` 与 `ensure_thread()`。

---

## 14. 智能指针与 delete_later

### 14.1 类型与工厂

```cpp
template <typename T>
using object_uptr = std::unique_ptr<T, object_delete_later>;

template <typename T>
using object_sptr = std::shared_ptr<T>;

template <typename T>
using object_wptr = std::weak_ptr<T>;

object_uptr<T> make_object_unique(Args&&...);
object_sptr<T> make_object_shared(Args&&...);  // 删除器为 delete_later
```

`object_delete_later` 在释放时调用 `p->delete_later()`，把销毁投递到亲和线程，与 Queued 槽串行，降低跨线程 UAF 风险。

```cpp
auto win = make_object_unique<Window>("Main");
connect(btn.clicked, win, &Window::onClicked);
// win 析构 → delete_later → 在亲和线程 delete
```

`object_get(p)`：从裸指针 / unique_ptr / shared_ptr 取裸指针。

### 14.2 delete_later 规则摘要

- 无亲和 loop：立即 `delete this`。
- 正在泵目标 loop，或调用方不在亲和线程：`post` 到目标再删；**post 失败则立即 `delete`**，避免泄漏。
- 亲和线程且当前未在泵：立即 `delete`（避免无 `exec` 时泄漏）。

---

## 15. 线程与生命周期注意事项

1. **不要直接使用 `event_loop`**：请用 `core_application` / `ensure_thread`（主线程）、继承 `object` 或 `worker_thread`（后台）代替。
2. **跨线程销毁**：先停相关 `worker_thread` / 排空队列，或依赖 `delete_later`。
3. **派生析构第一行 `invalidate()`**。
4. **BlockingQueued / `post_blocking`**：目标须**正在泵**（`is_pumping()`），不是仅 `is_running()`（接受 post）。  
   - **合法**：其他线程 → 正在 `run`/`exec` 的目标 loop。  
   - **非法 / 立即失败**：当前线程对本 loop 再 `post_blocking`（自死锁）；或目标空闲未泵（例如主线程未 `exec`）——避免永久挂起。  
5. **跨线程且无 loop**（如 `worker.stop()` 后）：Queued / Auto / Direct 均**跳过**，不会在发射线程直接调槽。  
6. **禁止在工作线程内 `worker_thread::stop()`**。  
7. 主线程建议先构造 `core_application`；需要 BlockingQueued 回主线程时主线程须在 `exec`/`process_events`。  
8. 堆对象优先 `object_uptr` / `object_sptr`；`connect` 不延长寿命。  
9. **亲和只绑 `utils::thread*`**：同线程判断比较 `object::thread()` 与 `ensure_thread()`；`object::loop()` 仅用于投递。  
10. Queued 槽抛异常会被事件循环吞掉，后续任务仍可执行。

---

## 16. 与 Qt 对照

| Qt | 本库 |
|----|------|
| `QObject` | `object` |
| `signals:` / 宏 | `signal<Args...>` 成员 |
| 槽 `void` / 任意返回 | 成员槽须 `slots_t`；lambda 不限 |
| `QObject::connect` | `connect` / `signal::connect` |
| `Qt::ConnectionType` | `connection_type` |
| `QMetaObject::invokeMethod` | `invoke`（`result` 取值） |
| `QCoreApplication` | `core_application` |
| `QThread` | `utils::thread` |
| `QThread::currentThread()` | `ensure_thread()` |
| `QThread` + 工作线程 | `worker_thread` |
| `QObject::thread()` | `object::thread()` → `thread*` |
| `deleteLater` | `delete_later` / `object_uptr` |
| `blockSignals` | `block_signals`（信号需 `signal{this}`） |
| 无公开 `invalidate` | 有 `invalidate()`（因更宽松的销毁模型） |

---

## 相关文件

- 实现：`concurrency/signal_and_slots/signal_and_slots.h`
- 示例：`demo/concurrency/signal_and_slots/sample.cpp`
- 错误类型：`reliability/result/expected.h`（`result` / `result_ok` / `result_err`）

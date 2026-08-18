# Signal & Slots（`utils::signal_and_slots`）

轻量 **Qt 风格** 信号/槽与事件循环库，C++17，header-only。

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
11. [core_application](#11-core_application)
12. [worker_thread](#12-worker_thread)
13. [智能指针与 delete_later](#13-智能指针与-delete_later)
14. [线程与生命周期注意事项](#14-线程与生命周期注意事项)
15. [与 Qt 对照](#15-与-qt-对照)

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

每个 `object` 绑定一个 `event_loop*`（仿 `QObject`）：

- 构造时绑定**当前线程**的默认 loop（`ensure_thread_loop()`）。
- 可用 `move_to_thread` 换到工作线程。
- `Queued` / `Auto`（跨线程）会把槽投递到接收者所属 loop。

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

构造时自动：`_loop = ensure_thread_loop()`。

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
| `void move_to_thread(event_loop*)` | 切换亲和 loop |
| `void move_to_thread(worker_thread*)` | |
| `void move_to_thread(worker_thread&)` | |
| `event_loop* thread() const` | 当前亲和 loop |

```cpp
worker_thread worker;
worker.start();

Window win;
win.move_to_thread(worker.loop());
// 之后 Queued 槽在 worker 线程执行
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
win.move_to_thread(worker.loop());

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
> 直接使用时需要手动设置线程亲和（`tls_default_loop`）、保证生命周期顺序、自行管理 `run()` / `stop()`。
> 遗漏任意一步都可能导致悬空指针或 UAF。
>
> **请优先使用以下三种封装方式：**
>
> | 场景 | 推荐方式 |
> |------|----------|
> | 应用程序主线程事件循环 | [`core_application`](#11-core_application) |
> | 继承 `object` 在当前线程接收槽 | 继承 `object`，直接 `connect`（自动亲和到构造线程） |
> | 后台独立事件循环线程 | [`worker_thread`](#12-worker_thread) |
>
> `event_loop` 本身保留给框架内部、测试以及极少数需要手写事件泵的特殊场景。

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
| `void post(task)` | 入队；loop 未 running 时可能失败（内部丢弃） |
| `bool post_blocking(task)` | 入队并阻塞等待完成；若当前正在泵**本** loop 则返回 `false`（防自死锁） |
| `timer_id post_delayed(duration, task)` | 延迟执行；`delay<=0` 等价 `post`，返回 id `0` |
| `timer_id post_periodic(duration, task)` | 周期执行（首次在 interval 之后） |
| `void cancel_timer(timer_id)` | 取消定时器 |
| `void run()` | 阻塞泵送，直到 `stop()` |
| `void stop()` | 结束 `run` |
| `bool is_running() const` | |
| `void process_events(optional<duration> budget = nullopt)` | 处理到期定时器与已排队任务。无 budget：不候未来 timer；有 budget：可 wait 到下一定时器 |

### 10.3 线程局部辅助

```cpp
event_loop* current_thread_loop() noexcept;  // 优先 running，否则 default
event_loop* ensure_thread_loop();            // 保证本线程有默认 loop
```

### 10.4 示例

```cpp
using namespace std::chrono_literals;

event_loop* loop = ensure_thread_loop();

loop->post([] { std::cout << "task\n"; });

auto id = loop->post_periodic(50ms, [] {
    std::cout << "tick\n";
});

// 非阻塞排空一点
loop->process_events(100ms);
loop->cancel_timer(id);
```

工作线程上通常由 `worker_thread` 调用 `run()`，不必手写。

> **`process_events()` 的适用场景**：测试、同步等待一批任务完成、或在没有完整事件循环的情况下手动排空队列。不适合长期运行的工作线程。

---

## 11. core_application

> **主线程应用程序的标准起点。**
> 构造后主线程所有 `object` 自动亲和到同一 loop，无需任何额外配置。

仿 `QCoreApplication`：在主线程注册默认 `event_loop`。

```cpp
class core_application {
public:
    core_application();           // ensure_thread_loop()
    event_loop* thread() const;
    int exec();                   // loop->run()，返回 0
};
```

```cpp
int main() {
    core_application app;
    // 此后本线程创建的 object 默认亲和到 app 的 loop
    // app.exec();  // 若需要主线程持续泵事件
    return 0;
}
```

即使不 `exec()`，只要构造过 `core_application`（或任意 `object` 触发了 `ensure_thread_loop`），主线程也有默认亲和 loop，可供 Direct / 同线程逻辑使用。跨线程 Queued 仍需要目标侧有人 `run`/`process_events`。

---

## 12. worker_thread

> **后台线程的标准方式。**
> 自动完成：创建 `event_loop`、设置工作线程亲和、在新线程 `run()`、停止时 `join()`。

一线程托管一个 `event_loop`（仿简易 `QThread`）。

| API | 说明 |
|-----|------|
| `void start()` | 创建 loop 并在新线程 `run()` |
| `void stop()` | `stop` loop 并 `join`。**禁止在工作线程内调用**（assert 硬失败） |
| `event_loop* loop() const` | |
| `bool is_running() const` | |

```cpp
worker_thread worker;
worker.start();

Window win;
win.move_to_thread(worker.loop());

connect(btn.clicked, &win, &Window::onClicked);  // Auto → 跨线程 Queued
btn.clicked.emit();

std::this_thread::sleep_for(50ms);
worker.stop();   // 务必在外线程 stop
```

---

## 13. 智能指针与 delete_later

### 13.1 类型与工厂

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

### 13.2 delete_later 规则摘要

- 无亲和 loop：立即 `delete this`。
- 正在泵目标 loop，或调用方不在亲和线程：`post` 到目标再删。
- 亲和线程且当前未在泵：立即 `delete`（避免无 `exec` 时泄漏）。

---

## 14. 线程与生命周期注意事项

1. **不要直接使用 `event_loop`**：请用 `core_application`（主线程）、继承 `object`（同线程槽）或 `worker_thread`（后台线程）代替。直接使用需手动管理亲和、生命周期和 `run`/`stop`，遗漏易造成悬空指针或 UAF。
2. **跨线程销毁**：先停相关 `worker_thread` / 排空队列，或依赖 `delete_later`。
3. **派生析构第一行 `invalidate()`**。
4. **BlockingQueued**：目标须 `run`；勿在正在泵的同一 loop 上对自己 `post_blocking` / `blocking_queued`。
5. **禁止在工作线程内 `worker_thread::stop()`**。
6. 主线程建议先构造 `core_application`。
7. 堆对象优先 `object_uptr` / `object_sptr`；`connect` 不延长寿命。
8. 跨线程 Direct 连接会降级为 Queued；无 loop 的 Queued/Blocking 会丢弃或失败。

---

## 15. 与 Qt 对照

| Qt | 本库 |
|----|------|
| `QObject` | `object` |
| `signals:` / 宏 | `signal<Args...>` 成员 |
| 槽 `void` / 任意返回 | 成员槽须 `slots_t`；lambda 不限 |
| `QObject::connect` | `connect` / `signal::connect` |
| `Qt::ConnectionType` | `connection_type` |
| `QMetaObject::invokeMethod` | `invoke`（`result` 取值） |
| `QCoreApplication` | `core_application` |
| `QThread` + 事件循环 | `worker_thread` + `event_loop` |
| `deleteLater` | `delete_later` / `object_uptr` |
| `blockSignals` | `block_signals`（信号需 `signal{this}`） |
| 无公开 `invalidate` | 有 `invalidate()`（因更宽松的销毁模型） |

---

## 相关文件

- 实现：`concurrency/signal_and_slots/signal_and_slots.h`
- 示例：`demo/concurrency/signal_and_slots/sample.cpp`
- 错误类型：`reliability/result/expected.h`（`result` / `result_ok` / `result_err`）

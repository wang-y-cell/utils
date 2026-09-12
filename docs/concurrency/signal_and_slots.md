# Signal & Slots 使用教程（`utils::signal_and_slots`）

轻量信号/槽与事件循环，C++20，header-only。  
接收者必须用智能指针；跨线程亲和在 **connect 时绑定 `thread*`**，并写入全局亲和表。

| 项 | 说明 |
|----|------|
| 头文件 | [`concurrency/signal_and_slots.h`](../../concurrency/signal_and_slots.h) |
| 命名空间 | `utils` |
| 依赖 | [`reliability/expected.h`](../../reliability/expected.h)（`result<T>`） |
| 可运行示例 | `demo/concurrency/signal_and_slots/sample.cpp`（目标：`demo_signal`） |

```bash
cmake --build build --target demo_signal
./build/demo_signal   # Windows: build\demo_signal.exe
```

手工编译时把**工程根目录**加入 include（不要只 `-I concurrency`）：

```bash
g++ -std=c++20 -O2 -I. main.cpp -o main.exe
```

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：同线程按钮与窗口](#4-教程-a同线程按钮与窗口)
5. [教程 B：连接类型怎么选](#5-教程-b连接类型怎么选)
6. [教程 C：后台 thread 跨线程](#6-教程-c后台-thread-跨线程)
7. [教程 D：取值 invoke](#7-教程-d取值-invoke)
8. [教程 E：断开、唯一连接、阻塞信号](#8-教程-e断开唯一连接阻塞信号)
9. [教程 F：寿命与智能指针](#9-教程-f寿命与智能指针)
10. [教程 G：亲和表与每槽不同线程](#10-教程-g亲和表与每槽不同线程)
11. [教程 H：定时器与 event_loop](#11-教程-h定时器与-event_loop)
12. [场景速查](#12-场景速查)
13. [与 Qt / 旧版对照](#13-与-qt--旧版对照)

---

## 1. 五分钟心智模型

```text
connect(sig, sptr/wptr, &T::slot, thread*, type)
        │
        ├─ 连接条目里固定 entry._target = thread*
        └─ 可选：写入亲和表 (receiver*, method) → thread*

sig.emit(...)
   ├─ Direct                         → 在 emit 线程立刻调槽
   ├─ Auto + 无 target / 同线程      → Direct
   ├─ Auto / Queued + 跨线程有 target → post 到 target->loop()
   └─ BlockingQueued                 → post_blocking（目标须正在泵）

槽执行前：weak_ptr.lock()；失败则跳过（接收者已毁）
```

硬规则：

1. **接收者只能是 `sptr` / `wptr`**（`uptr`、裸指针、栈对象都不能 `connect`）。
2. **必须用自由函数 `utils::connect(...)`**；`signal::connect` 为 private。
3. **跨线程亲和在 connect 时绑定**，不可中途 `move_to_thread`（已删除）。
4. **只有一种 `thread`**：spawn OS 线程 + `event_loop`（`using worker_thread = thread`）。无需 `core_application`。
5. **连接不延长寿命**：内部存 `weak_ptr`；对象销毁后槽不再执行。

推荐入口：

| 你想… | 用 |
|------|----|
| 定义信号 | 成员 `signal<>` / `signal<Args...>`（无需绑 owner） |
| 定义槽 | 成员返回 `slots_t` / `slots_t<T>`；或 lambda |
| 接收者 | `auto w = std::make_shared<Window>();` |
| 连接 | `connect(sig, w, &Window::on_x [, &worker] [, type])` |
| 后台跑槽 | `thread worker; worker.start();` + connect 时传入 `&worker` |
| 发射 | `sig.emit(...)` 或 `sig(...)` |
| 同步取值 | `invoke(sptr, &T::slot, &worker, connection_type::blocking_queued, ...)` |

别名：

```cpp
template<class T> using sptr = std::shared_ptr<T>;
template<class T> using uptr = std::unique_ptr<T>;  // 通用别名；不能当 connect 接收者
template<class T> using wptr = std::weak_ptr<T>;
```

---

## 2. 全部可行操作一览

### 2.1 定义信号与槽

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 声明信号 | `signal<> clicked;` / `signal<int> changed;` | 无需 `{this}` |
| 成员槽 | `slots_t<> onClick();` / `slots_t<int> value();` | 成员槽**必须**返回 `slots_t` |
| lambda 槽 | `connect(sig, sp, [](...){...});` | 不要求 `slots_t` |

> 见 [教程 A](#4-教程-a同线程按钮与窗口)。

### 2.2 连接与断开

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 连成员槽 | `connect(sig, sp, &T::slot);` | 默认 `automatic`；查亲和表 |
| 指定线程 | `connect(sig, sp, &T::slot, &worker);` | 写入亲和表并固定连接 target |
| 指定类型 | `connect(..., connection_type::queued);` | 见 [教程 B](#5-教程-b连接类型怎么选) |
| 唯一连接 | `connect(..., unique_connection);` | 同接收者+同成员槽只连一次；**对 lambda 无效** |
| `wptr` | `connect(sig, wp, &T::slot);` | 已过期则连不上或之后跳过 |
| 拿句柄 | `connection c = connect(...);` | 可 `c.disconnect()`；析构**不断**连 |
| RAII | `scoped_connection sc{connect(...)};` | 离开作用域**自动**断 |
| 可不保存返回值 | `connect(...);` | 连接仍留在 signal 里，直到 signal 析构 / 批量断 |
| 按接收者断 | `sig.disconnect(sp);` | 该接收者在本信号上的全部连接 |
| 全断 | `sig.disconnect_all();` | 信号析构也会做 |

**禁止：** `sig.connect(...)`（private）、`connect(sig, &stack_obj, ...)`、`connect` 接收 `uptr`。

### 2.3 发射与阻塞

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 发射 | `sig.emit(args...);` / `sig(args...);` | 按值 decay-copy |
| 阻塞本信号 | `sig.block_signals(true);` | 仅作用于该 `signal` |

### 2.4 线程与事件循环

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 后台线程 | `thread w; w.start();` … `w.stop();` | 禁止在 worker 自己线程里 `stop` |
| 当前是否在 worker | `current_thread()` | 主线程为 `nullptr` |
| 投递 | `w.loop()->post(fn);` | `post_blocking` / 定时器见教程 H |
| 临时 loop | `event_loop loop; loop.process_events();` | 可独立用，不绑亲和 |

### 2.5 亲和表

实现为两级 `unordered_map`：`receiver* → (method_key → thread*)`；`method_key = type_index + PMF 字节`。

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 绑定 | `bind_slot_affinity(sp.get(), &T::slot, &w);` 或 connect 带 `thread*` | 同一槽重复 bind 会覆盖 |
| 查找 | `find_slot_affinity(sp.get(), &T::slot);` | `invoke` / 无 thread 的 connect 会查 |
| 清除 | `clear_slot_affinity(sp.get());` | |

### 2.6 invoke

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 成员槽（查表） | `invoke(sp, &T::slot, args...);` | |
| 成员槽（显式线程） | `invoke(sp, &T::slot, &w, type, args...);` | |
| callable | `invoke(&w, type, fn, args...);` | **必须**带 `thread*` |
| fire-and-forget | `invoke(&w, std::function<void()>{...});` | |

`Queued` / 跨线程 `Auto` 无法同步取 `result` → 错误码（如 `operation_in_progress`）。

---

## 3. 异常操作与 Tips（必读）

| 不要这样做 | 为什么 | 正确做法 |
|------------|--------|----------|
| `sig.connect(...)` | private | `utils::connect(sig, ...)` |
| 栈对象 / 裸指针当接收者 | 无重载；无法 weak 寿命 | `make_shared` |
| `uptr` 当接收者 | 不能安全得到 weak | 改用 `sptr` |
| 临时 `scoped_connection{connect(...)}` 又期望连接保留 | 语句结束就断连 | 存成员 / 用 `connection` / 故意丢弃 `connection` |
| Queued 但不传 `thread*` 且表中无绑定 | target 空，槽被跳过 | connect 时传入 `&worker` 或先 `bind_slot_affinity` |
| 在 worker 线程里 `worker.stop()` | 硬失败 | 在其它线程 stop |
| BlockingQueued 而目标未泵 | 失败/跳过，不死等 | 先 `start` 并等到 `is_pumping` |
| Direct 跨线程摸无锁状态 | 数据竞争 | Queued 到固定 worker，或自备同步 |
| 依赖 `core_application` / `move_to_thread` / 继承 `object` | 已删除 | 见本文新模型 |

Tips：

- **主线程不必 `exec`。** 槽的 Queued 执行面在 `thread`（worker）上；主线程可只 `emit`。
- **丢弃 `connection` 返回值可以**：连接仍在 signal 中；需要中途断再保存句柄。
- **`scoped_connection` 非必须**：与 signal/接收者同寿命时，存 `connection` 或不存都行。
- **同一对象不同槽可绑不同 `thread*`**（比 Qt 一对象一线程更灵活）；共享可变状态需自备同步。

---

## 4. 教程 A：同线程按钮与窗口

```cpp
#include "concurrency/signal_and_slots.h"
using namespace utils;

class Button {
public:
    signal<> clicked;
};

class Window {
public:
    slots_t<> on_click() {
        // ...
        return {};
    }
};

int main() {
    Button btn;
    auto win = std::make_shared<Window>();

    // 无 thread*：Automatic → Direct（同线程立刻调）
    connect(btn.clicked, win, &Window::on_click);

    btn.clicked.emit();
    // 或 btn.clicked();
}
```

要点：

1. 接收者 `shared_ptr`。
2. 用自由函数 `connect`。
3. 不传 worker 时默认同线程 Direct。

---

## 5. 教程 B：连接类型怎么选

| `connection_type` | 行为 |
|-------------------|------|
| `direct` | 始终在 **emit 线程**同步调用（跨线程也如此，调用方自担安全） |
| `queued` | 投递到连接固定的 `thread*` 的 loop；无 target 则**跳过** |
| `blocking_queued` | 投递并等待；目标须 `is_pumping`；同线程泵本 loop 会拒（防自死锁） |
| `automatic` | 同线程或无 target → Direct；跨线程有 target → Queued |

```cpp
connect(sig, win, &Window::on_x, &worker, connection_type::queued);
connect(sig, win, &Window::on_y, connection_type::direct);
```

---

## 6. 教程 C：后台 thread 跨线程

```cpp
thread worker;
worker.start();

Button btn;
auto win = std::make_shared<Window>();

// 写入亲和表，Queued 投到 worker
connect(btn.clicked, win, &Window::on_click, &worker);

btn.clicked.emit();   // 主线程 emit → worker 上跑槽

// ...
worker.stop();        // 勿在 worker 自己的线程里调用
```

`worker.stop()` 后 loop 为空；再 Queued emit 会安全跳过。再次 `start()` 后，**已固定的 `thread*` 句柄仍是同一个**，一般不必重连（新 loop 由该 `thread` 对象持有）。

别名：`worker_thread` 即 `thread`。

---

## 7. 教程 D：取值 invoke

```cpp
auto win = std::make_shared<Window>();
thread worker;
worker.start();

// 显式线程 + 阻塞取回
auto r = invoke(win, &Window::name_len, &worker,
                connection_type::blocking_queued);
if (r) { /* *r */ }

// 先 bind / connect 写过亲和表，可省略 thread*
bind_slot_affinity(win.get(), &Window::name_len, &worker);
auto r2 = invoke(win, &Window::name_len, connection_type::blocking_queued);

// lambda / 自由函数：必须带 thread*
auto sum = invoke(&worker, connection_type::blocking_queued,
                  [](int a, int b) { return a + b; }, 40, 2);

invoke(&worker, [&]{ /* fire-and-forget */ });
```

---

## 8. 教程 E：断开、唯一连接、阻塞信号

```cpp
auto win = std::make_shared<Window>();

auto c1 = connect(sig, win, &Window::on_x, unique_connection);
auto c2 = connect(sig, win, &Window::on_x, unique_connection);
// c2.connected() == false

sig.block_signals(true);
sig.emit(...);   // 不调槽
sig.block_signals(false);

c1.disconnect();
// 或
sig.disconnect(win);
sig.disconnect_all();

{
    scoped_connection sc{connect(sig, win, &Window::on_y)};
} // 自动断开
```

---

## 9. 教程 F：寿命与智能指针

```cpp
signal<> sig;
{
    auto w = std::make_shared<Window>();
    connect(sig, w, &Window::on_x);  // 内部 weak
    sig.emit();                      // 会调
} // w 销毁
sig.emit();                          // lock 失败，跳过，不崩溃
```

- **不要**用栈对象当接收者。  
- 连接 **不**延长 `shared_ptr` 寿命（避免环）。  
- `wptr` 连接：lock 失败则无法建立有效调用。

---

## 10. 教程 G：亲和表与每槽不同线程

```cpp
thread w1, w2;
w1.start();
w2.start();

auto obj = std::make_shared<Multi>();
signal<> a, b;

connect(a, obj, &Multi::slot_a, &w1, connection_type::queued);
connect(b, obj, &Multi::slot_b, &w2, connection_type::queued);

// find_slot_affinity(obj.get(), &Multi::slot_a) == &w1
```

同一对象两槽跑在不同线程时，**不要无锁共享可变成员**，或自行加锁。

也可用：

```cpp
bind_slot_affinity(obj.get(), &Multi::slot_a, &w1);
connect(a, obj, &Multi::slot_a, connection_type::queued); // 查表得到 w1
```

---

## 11. 教程 H：定时器与 event_loop

```cpp
thread worker;
worker.start();

auto id = worker.loop()->post_periodic(std::chrono::milliseconds(30), []{
    // ...
});
// ...
worker.loop()->cancel_timer(id);
worker.stop();
```

独立 loop（测试 / 临时排空）：

```cpp
event_loop loop;
loop.post([]{});
loop.process_events();
loop.process_events(std::chrono::milliseconds(100)); // 可候定时器
```

长期跑泵：用 `thread::start()`，不要依赖已删除的 `core_application::exec`。

---

## 12. 场景速查

| 场景 | 做法 |
|------|------|
| 同线程回调 | `connect(sig, sp, &T::slot)` |
| 后台处理 | `thread` + `connect(..., &worker [, queued])` |
| 同步取返回值 | `invoke(..., blocking_queued, ...)` |
| 临时连接 | `scoped_connection` |
| 长期连接、很少断 | 可不保存 `connect` 返回值 |
| 需要中途断 | 存 `connection` 或 `disconnect(sp)` |
| 每槽不同线程 | connect 时传入不同 `thread*` |
| 定时任务 | `thread::loop()->post_periodic` |

---

## 13. 与 Qt / 旧版对照

| Qt / 旧 utils | 现在 |
|---------------|------|
| 继承 `QObject` / `object` | **不需要**；接收者普通类 + `sptr` |
| `moveToThread` / `move_to_thread` | **已删除**；connect 时绑 `thread*` |
| `QCoreApplication` / `core_application` | **不需要** |
| `QThread` 当前线程 + worker | 单一 `thread`（worker）；`current_thread()` 仅 TLS |
| 一对象一亲和线程 | **每槽**可不同线程（亲和表） |
| `sender.connect(receiver, SLOT)` | `utils::connect(sig, sptr, &T::slot, ...)` |
| `deleteLater` / `object_uptr` | **已删除**；用 `shared_ptr` 寿命 |
| 裸指针接收者 | **不支持** |

---

## 相关文件

- 实现：[`concurrency/signal_and_slots.h`](../../concurrency/signal_and_slots.h)
- 示例：`demo/concurrency/signal_and_slots/sample.cpp`
- 测试：`tests/signal_and_slots_test.cpp`

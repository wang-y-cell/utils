# deadline 使用教程（`utils::deadline`）

基于 `std::chrono::steady_clock` 的**绝对截止时间**。协作取消请直接用标准库 `std::stop_token` / `std::stop_source`。  
常与 [retry](./retry.md) 组合；错误通道见 [result](./result.md)。

本文以**怎么用**为主：可行操作、异常 Tips、分场景教程。

| 项 | 说明 |
|----|------|
| 头文件 | [`reliability/time/deadline.h`](../../reliability/time/deadline.h) |
| 命名空间 | `utils` |
| 伞头 | 已由 `utils/utils.h` / `reliability/reliability.h` 重导出 |
| 可运行示例 | `demo/reliability/time/demo.cpp`（目标：`demo_deadline`） |

```bash
cmake --build build --target demo_deadline
./build/demo_deadline   # Windows: build\demo_deadline.exe
```

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：after + 轮询到过期](#4-教程-aafter--轮询到过期)
5. [教程 B：remaining / remaining_as](#5-教程-bremaining--remaining_as)
6. [教程 C：never 与 at](#6-教程-cnever-与-at)
7. [教程 D：交给 retry](#7-教程-d交给-retry)
8. [场景速查](#8-场景速查)
9. [和 stop_token 怎么分工](#9-和-stop_token-怎么分工)

---

## 1. 五分钟心智模型

```text
deadline = 墙上的一个绝对时刻（steady_clock）
   │
   ├─ expired()     → now >= 截止？
   └─ remaining()   → 还剩多久（已过期则为 0）
```

三条硬规则：

1. **用的是 `steady_clock`**（单调），不受系统改时影响。
2. **`deadline` 只管「何时到期」**；真正停下来要你在循环里查，或交给 `retry`。
3. **协作取消用 `stop_token`**，不要把 deadline 当成取消令牌本身。

推荐入口：

| 你想… | 用 |
|------|----|
| 从现在起再过一段时间 | `deadline::after(200ms)` |
| 某个绝对时刻 | `deadline::at(tp)` |
| 永不截止 | `deadline::never()` |
| 还剩多久 | `remaining()` / `remaining_as<ms>()` |
| 重试总时限 | `retry(op, policy, token, d)` |

---

## 2. 全部可行操作一览

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 相对截止 | `auto d = deadline::after(200ms);` | `now + duration` |
| 绝对截止 | `deadline::at(tp)` | `tp` 为 `steady_clock::time_point` |
| 永不 | `deadline::never()` | 内部 `time_point::max()` |
| 默认构造 | `deadline{}` | 同 `never` |
| 是否过期 | `d.expired()` | |
| 剩余 | `d.remaining()` | 过期 → `0` |
| 剩余（指定单位） | `d.remaining_as<std::chrono::milliseconds>()` | |
| 原始时刻 | `d.time_point_value()` | |

---

## 3. 异常操作与 Tips（必读）

### 3.1 禁止 / 易踩坑

| 异常操作 | 后果 | 正确做法 |
|----------|------|----------|
| 用 `system_clock` 自己算再硬套 | 调时会跳 | 继续用本类型的 `after`/`at`（steady） |
| 构造一次后以为会「自动停线程」 | 什么都不会发生 | 循环检查或传给 `retry` |
| 把 deadline 当取消广播 | 其他线程收不到「请停」 | 另传 `stop_source` / `stop_token` |
| 过期后还假设 `remaining()` 为负 | 实际是 **0** | 用 `expired()` 判断 |

### 3.2 Tips

**Tip 1 — `after` 是构造瞬间拍板**

- `deadline::after(200ms)` 的截止点在调用当下算死；之后不会「再延长」除非你重新造一个。

**Tip 2 — 与 retry**

- `retry` 会在每次循环前查 `expired()`，并把 sleep 截到 `remaining()`。

**Tip 3 — 与 channel / 自己的阻塞 API**

- 本库 channel 当前是无限等 `recv`；要超时请在外层用 deadline 轮询 `try_recv`，或自写带超时的等待。

---

## 4. 教程 A：after + 轮询到过期

```cpp
#include "reliability/time/deadline.h"
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

auto d = deadline::after(40ms);
while (!d.expired()) {
    // 干一点活…
    std::this_thread::sleep_for(5ms);
}
```

---

## 5. 教程 B：remaining / remaining_as

```cpp
auto d = deadline::after(200ms);
auto left = d.remaining();
auto left_ms = d.remaining_as<std::chrono::milliseconds>().count();
```

适合：把「还能等多久」传给下层 sleep / 条件变量 wait_for。

---

## 6. 教程 C：never 与 at

```cpp
auto never = deadline::never();
// never.expired() 在正常运行期内为 false

auto tp = std::chrono::steady_clock::now() + 1s;
auto d = deadline::at(tp);
```

---

## 7. 教程 D：交给 retry

```cpp
#include "reliability/retry/retry.h"
#include "reliability/time/deadline.h"

auto d = deadline::after(200ms);
auto r = retry(
    op,
    retry_policy::exponential(10, 5ms),
    std::stop_token{},
    d);
```

详见 [retry.md](./retry.md)。

---

## 8. 场景速查

| 我想… | 步骤 |
|------|------|
| 循环最多跑 N 毫秒 | A：`after` + `!expired()` |
| 把剩余时间传给 sleep | B：`remaining` / `remaining_as` |
| 无截止默认参数 | C：`never()` |
| 重试总预算 | D：传给 `retry` |
| 用户点取消 | 另用 `stop_token`（见下节） |

---

## 9. 和 stop_token 怎么分工

| 机制 | 表达 |
|------|------|
| `deadline` | 「到点了，别再等了」（时间） |
| `stop_token` | 「外部请停」（协作取消） |

两者正交，`retry` 两个都能传：任一触发都会结束重试。

---

## 相关文件

- 实现：`reliability/time/deadline.h`
- 示例：`demo/reliability/time/demo.cpp`
- 测试：`tests/deadline_test.cpp`
- 相关：[retry.md](./retry.md)、[result.md](./result.md)

# retry 使用教程（`utils::retry` / `retry_policy`）

对返回 `result` / `expected` 的操作做有限次重试，支持固定/指数退避、抖动、`stop_token` 与 `deadline`。  
错误类型见 [result](./result.md)；截止见 [deadline](./time.md)。

本文以**怎么用**为主：可行操作、异常 Tips、分场景教程。

| 项 | 说明 |
|----|------|
| 头文件 | [`reliability/retry/retry.h`](../../reliability/retry/retry.h) |
| 命名空间 | `utils` |
| 依赖 | `expected.h`、`deadline.h`、`<stop_token>` |
| 伞头 | 已由 `utils/utils.h` / `reliability/reliability.h` 重导出 |
| 可运行示例 | `demo/reliability/retry/demo.cpp`（目标：`demo_retry`） |

```bash
cmake --build build --target demo_retry
./build/demo_retry   # Windows: build\demo_retry.exe
```

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：指数退避直到成功](#4-教程-a指数退避直到成功)
5. [教程 B：固定间隔](#5-教程-b固定间隔)
6. [教程 C：部分错误不重试](#6-教程-c部分错误不重试)
7. [教程 D：stop_token 取消](#7-教程-dstop_token-取消)
8. [教程 E：deadline 截断等待](#8-教程-edeadline-截断等待)
9. [场景速查](#9-场景速查)
10. [和线程池 / executor 怎么配合](#10-和线程池--executor-怎么配合)

---

## 1. 五分钟心智模型

```text
retry(op, policy [, should_retry] [, token] [, deadline])
        │
        ├─ op() 成功 → 立刻返回
        ├─ 失败且不可重试 → 返回该失败
        ├─ 失败且可重试 → sleep(delay) → 再试
        └─ 次数用尽 / 取消 / 截止 → 返回最后失败或 canceled
```

三条硬规则：

1. **`op` 必须返回 `expected` / `result`**；业务失败用 `result_err`，不要靠异常做重试控制流。
2. **`retry` 在当前线程同步循环**；不会自己开线程。
3. **请保证 `op` 幂等或可安全重复**（否则重试会放大副作用）。

推荐入口：

| 你想… | 用 |
|------|----|
| 指数退避 | `retry_policy::exponential(n, initial)` |
| 固定间隔 | `retry_policy::fixed(n, delay)` |
| 某些错误不重试 | 第三个参数 `should_retry` |
| 可取消 | 传入 `std::stop_token` |
| 总时限 | 传入 `deadline` |

---

## 2. 全部可行操作一览

### 2.1 策略

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 固定 | `retry_policy::fixed(5, 10ms)` | 间隔恒定；无 jitter |
| 指数 | `retry_policy::exponential(5, 10ms)` | 默认可 jitter，可封顶 |
| 手调字段 | `policy.max_attempts` 等 | `multiplier` / `max_delay` / `jitter` |
| 算下一次等待 | `policy.delay_after_failure(i)` | 耗尽 → `nullopt` |

### 2.2 retry 函数

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 默认全可重试 | `retry(op, policy)` | 失败一律退避再试 |
| 带谓词 | `retry(op, policy, should_retry)` | `should_retry(error)` |
| 取消 | 再传 `stop_token` | 停止后返回上次失败或 `operation_canceled` |
| 截止 | 再传 `deadline` | 过期同上；睡眠截到 `remaining()` |

```cpp
retry(op, policy);
retry(op, policy, should_retry);
retry(op, policy, token, deadline);
retry(op, policy, should_retry, token, deadline);
```

---

## 3. 异常操作与 Tips（必读）

### 3.1 禁止 / 易踩坑

| 异常操作 | 后果 | 正确做法 |
|----------|------|----------|
| `op` 抛异常当失败 | 异常冒出，retry 接不住 | `op` 返回 `result_err` |
| 非幂等写操作直接 retry | 重复下单 / 重复写入 | 只对可安全重试的调用用；或谓词过滤 |
| 以为 retry 会异步跑 | 调用线程被 sleep 堵住 | 要异步请自己 `post` 到线程池 |
| 错误类型无法构造 canceled | 取消路径编译不过 / 难返回 | 优先 `result`（`error_code`） |
| `max_attempts == 0` | 实现里当成 **1** | 想「不试」就别调 retry |

### 3.2 Tips

**Tip 1 — `failure_index` 从 0 起**

- 第一次失败后算 delay 用 index `0`；次数用尽 `delay_after_failure` 返回 `nullopt`。

**Tip 2 — jitter**

- `exponential` 默认 `jitter = true`：实际睡眠在 `[0, base]` 均匀抽，减惊群。
- `fixed` 默认关 jitter。

**Tip 3 — 取消时返回什么**

- 若已经失败过：优先返回**上次失败**。
- 若还没跑过 / 没有 last_fail：尽量构造 `operation_canceled`。

**Tip 4 — 睡眠会被 deadline 截断**

- `sleep_for = min(delay, deadline.remaining())`。

---

## 4. 教程 A：指数退避直到成功

```cpp
#include "reliability/retry/retry.h"
using namespace utils;
using namespace std::chrono_literals;

int calls = 0;
auto ok = retry(
    [&]() -> result<int> {
        ++calls;
        if (calls < 3) return result_err(std::errc::connection_reset);
        return result_ok(100);
    },
    retry_policy::exponential(5, 5ms));
```

---

## 5. 教程 B：固定间隔

```cpp
auto r = retry(
    []() -> result<int> { return fetch(); },
    retry_policy::fixed(5, 10ms));
```

适合：上游明确「隔固定时间再问」。

---

## 6. 教程 C：部分错误不重试

```cpp
auto r = retry(
    [&]() -> result<int> { return call_rpc(); },
    retry_policy::fixed(5, 1ms),
    [](const std::error_code& ec) {
        // 参数错误不重试
        return ec != std::make_error_code(std::errc::invalid_argument);
    });
```

---

## 7. 教程 D：stop_token 取消

```cpp
std::stop_source source;
source.request_stop();

auto canceled = retry(
    []() -> result<int> { return result_ok(1); },
    retry_policy::fixed(3, 1ms),
    source.get_token());
// canceled 多为 operation_canceled（若尚无上次失败）
```

---

## 8. 教程 E：deadline 截断等待

```cpp
#include "reliability/time/deadline.h"

auto d = deadline::after(200ms);
auto r = retry(
    op,
    retry_policy::exponential(10, 5ms),
    std::stop_token{},
    d);
```

细节见 [time.md](./time.md)。

---

## 9. 场景速查

| 我想… | 步骤 |
|------|------|
| RPC 抖一下再通 | A：`exponential` |
| 轮询固定节拍 | B：`fixed` |
| 4xx 不重试、5xx 重试 | C：`should_retry` |
| UI/停机可取消 | D：`stop_token` |
| 整段最多 200ms | E：`deadline` |
| 异步重试 | 自己把 `retry(...)` 丢进 [thread_pool](../concurrency/thread_pool.md) |

---

## 10. 和线程池 / executor 怎么配合

`retry` **不**创建线程。常见拼法：

```cpp
pool.add_task([&] {
    auto r = retry(op, retry_policy::exponential(5, 10ms));
    // 处理 r
});
```

或业务只依赖「在哪执行」时用 [executor](../concurrency/executor.md) 的 `post`。

---

## 相关文件

- 实现：`reliability/retry/retry.h`
- 示例：`demo/reliability/retry/demo.cpp`
- 测试：`tests/retry_test.cpp`
- 相关：[result.md](./result.md)、[time.md](./time.md)

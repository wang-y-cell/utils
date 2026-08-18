# retry / retry_policy

对返回 `result` / `expected` 的操作做有限次重试，支持固定/指数退避、抖动、`stop_token` 与 `deadline`。

| 项 | 说明 |
|----|------|
| 头文件 | [`reliability/retry/retry.h`](../reliability/retry/retry.h) |
| 命名空间 | `utils` |
| 依赖 | `expected.h`、`deadline.h`、`<stop_token>` |
| 伞头 | 已由 `utils/utils.h` 重导出 |
| 示例 | `demo/reliability/retry/demo.cpp`（`demo_retry`） |

---

## 目录

1. [快速入门](#1-快速入门)
2. [retry_policy](#2-retry_policy)
3. [retry 函数](#3-retry-函数)
4. [取消与截止](#4-取消与截止)
5. [注意事项](#5-注意事项)

---

## 1. 快速入门

```cpp
#include "reliability/retry/retry.h"
using namespace utils;
using namespace std::chrono_literals;

auto r = retry(
    []() -> result<int> {
        return fetch();  // 失败则 result_err(...)
    },
    retry_policy::exponential(5, 10ms));

if (r) std::cout << *r << "\n";
```

只描述**策略**；真正的 RPC/IO 由传入的 lambda 完成。

---

## 2. retry_policy

```cpp
class retry_policy {
public:
    std::size_t max_attempts = 3;
    clock::duration initial_delay = 50ms;
    double multiplier = 2.0;
    clock::duration max_delay = 5s;
    bool jitter = true;

    static retry_policy fixed(std::size_t attempts, duration delay);
    static retry_policy exponential(std::size_t attempts, duration initial,
                                    double mult = 2.0, duration max_d = 5s);

    std::optional<duration> delay_after_failure(std::size_t failure_index) const;
};
```

| 工厂 | 含义 |
|------|------|
| `fixed(n, delay)` | 最多 `n` 次，间隔固定 |
| `exponential(...)` | 指数退避，可封顶 `max_delay`，默认可 jitter |

---

## 3. retry 函数

```cpp
// 带「是否可重试」谓词
template <class Op, class Policy, class Pred>
auto retry(Op op, Policy policy, Pred should_retry,
           std::stop_token token = {},
           deadline d = deadline::never());

// 默认：失败一律可重试（直到策略用尽）
template <class Op, class Policy>
auto retry(Op op, Policy policy,
           std::stop_token token = {},
           deadline d = deadline::never());
```

### 行为摘要

1. `op` 必须返回 `expected` / `result`。
2. 成功 → 立即返回。
3. 失败且 `should_retry(error) == false` → 不再重试，返回该失败。
4. 否则按 `delay_after_failure` 睡眠后再次尝试。
5. 次数用尽 → 返回最后一次失败。

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

## 4. 取消与截止

| 条件 | 结果 |
|------|------|
| `stop_token` 请求停止 | 返回上次失败，或可构造的 `operation_canceled` |
| `deadline` 过期 | 同上 |
| 睡眠 | 会被截断到 `deadline.remaining()` |

```cpp
std::stop_source source;
auto d = deadline::after(200ms);
auto r = retry(op, retry_policy::exponential(10, 5ms),
               source.get_token(), d);
```

---

## 5. 注意事项

1. 重试的是**整个** `op`；请保证 `op` 幂等或可安全重复。
2. 错误类型需能与 `error_code` / `errc` 协作（取消路径）。
3. 与 [executor](./executor.md) / [thread_pool](./thread_pool.md) 无关：`retry` 在**当前线程**同步循环；异步请自行投递。

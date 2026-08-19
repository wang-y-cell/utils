# result / expected 使用教程（`utils::result` / `utils::expected`）

用返回值表示成功或失败，把业务错误从异常控制流里拆出来。  
重试见 [retry](./retry.md)；截止时间见 [deadline](./time.md)。

本文以**怎么用**为主：可行操作、异常 Tips、分场景教程。

| 项 | 说明 |
|----|------|
| 头文件 | [`reliability/result/expected.h`](../../reliability/result/expected.h) |
| 命名空间 | `utils` |
| 伞头 | 已由 `utils/utils.h` / `reliability/reliability.h` 重导出 |
| 可运行示例 | `demo/reliability/result/expected_demo.cpp`（目标：`demo_expected`） |

```bash
cmake --build build --target demo_expected
./build/demo_expected   # Windows: build\demo_expected.exe
```

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：最小检查与取值](#4-教程-a最小检查与取值)
5. [教程 B：工厂与 void 结果](#5-教程-b工厂与-void-结果)
6. [教程 C：and_then / transform 链式](#6-教程-cand_then--transform-链式)
7. [教程 D：or_else / transform_error 兜底](#7-教程-dor_else--transform_error-兜底)
8. [教程 E：自定义错误类型](#8-教程-e自定义错误类型)
9. [场景速查](#9-场景速查)
10. [和 retry / 异常怎么选](#10-和-retry--异常怎么选)

---

## 1. 五分钟心智模型

```text
函数返回 expected<T, E> / result<T>
        │
        ├─ has_value() == true  → 持有成功值 T（void 特化无载荷）
        └─ has_value() == false → 持有错误 E
```

三条硬规则：

1. **业务失败请 `return result_err(...)` / `unexpected`**，不要靠 throw 当日常分支。
2. **日常取值用 `if (r) { use(*r); }`**；`value()` 只在「无值就是编程错误」时用（会抛）。
3. **本仓库业务优先 `result<T>`**（`E = std::error_code`），与 `retry` 等一致。

推荐入口：

| 你想… | 用 |
|------|----|
| 成功带值 | `return result_ok(x);` |
| 失败 | `return result_err(std::errc::...);` |
| 无载荷成功/失败 | `result<void>` + `result_ok()` / `result_err` |
| 成功继续算 | `and_then` / `transform` |
| 失败兜底 | `or_else` / `value_or` |

---

## 2. 全部可行操作一览

### 2.1 类型

| 类型 | 说明 |
|------|------|
| `expected<T, E>` | 成功 `T` 或错误 `E` |
| `expected<void, E>` | 成功无载荷 |
| `result<T>` | `expected<T, std::error_code>` |
| `unexpected<E>` | 显式错误包装，消歧义 |
| `unexpect` | 标签：`expected(unexpect, ...)` |
| `bad_expected_access` | 对空值调 `value()` 时抛出 |

### 2.2 构造 / 工厂

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 成功（result） | `result_ok(42)` / `result_ok()` | 有值 / void |
| 失败（result） | `result_err(std::errc::io_error)` | 得到 `unexpected`，赋给 `result` |
| 通用成功 | `ok(1)` / `ok()` | 默认 `E = error_code` |
| 通用失败包装 | `err(e)` / `unexpected(e)` | |
| 标签构造 | `expected<T,E>(unexpect, ...)` | 原位造错误 |

### 2.3 查询与访问

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 是否成功 | `if (r)` / `r.has_value()` | |
| 取值（不检查） | `*r` / `r->` | 仅在已知成功时 |
| 取值（检查） | `r.value()` | 失败抛 `bad_expected_access` |
| 取错误 | `r.error()` | 仅在失败时；成功态属编程错误 |
| 默认值 | `r.value_or(0)` | 失败返回默认 |
| 默认错误 | `r.error_or(ec)` | 成功返回默认错误 |

### 2.4 单子式组合

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 成功继续 | `r.and_then(f)` | `f` 返回新的 `expected` |
| 映射成功值 | `r.transform(f)` | 失败原样传递 |
| 失败处理 | `r.or_else(f)` | 成功则跳过 |
| 映射错误 | `r.transform_error(f)` | 成功原样传递 |

---

## 3. 异常操作与 Tips（必读）

### 3.1 禁止 / 易踩坑

| 异常操作 | 后果 | 正确做法 |
|----------|------|----------|
| 用 throw 表达「参数不对 / 找不到」 | 异常当控制流，难组合 | `return result_err(...)` |
| 未检查就 `*r` / `r->` | UB / 读错侧 | 先 `if (r)` |
| 成功态调 `error()` | assert / 未定义行为预期 | 仅失败时调 |
| 把 `value()` 当日常 API | 失败就抛，难测难链 | 优先 `if` + `*` / `value_or` |
| `T` 与 `E` 同型时直接塞值 | 构造歧义 | 用 `unexpected` / `unexpect` |

### 3.2 Tips

**Tip 1 — `result_err` 本身不是完整 `result`**

- 它返回 `unexpected`；写成 `result<int> r = result_err(...);` 才成为失败态。

**Tip 2 — 链式会短路**

- `and_then` / `transform` 在失败时**不调用**回调，错误原样往下传。

**Tip 3 — void 特化**

- `result<void>` 成功无 `*` 载荷；`and_then` / `transform` 的回调无参数。

**Tip 4 — 和 retry**

- `retry` 要求 `op` 返回 `expected`/`result`；策略见 [retry.md](./retry.md)。

---

## 4. 教程 A：最小检查与取值

```cpp
#include "reliability/result/expected.h"
#include <iostream>

using namespace utils;

result<int> parse_positive(std::string_view s) {
    if (s.empty()) return result_err(std::errc::invalid_argument);
    // ...
    return result_ok(42);
}

int main() {
    auto r = parse_positive("21");
    if (!r) {
        std::cerr << r.error().message() << "\n";
        return 1;
    }
    std::cout << *r << "\n";
}
```

---

## 5. 教程 B：工厂与 void 结果

```cpp
result<int> a = result_ok(1);
result<int> b = result_err(std::errc::timed_out);
result<void> save_ok = result_ok();
result<void> save_bad = result_err(std::errc::io_error);

auto x = ok(1);     // expected<int, error_code>
auto y = ok();      // expected<void, error_code>
```

决策：

```text
业务错误通道？ → 优先 result<T> + result_ok / result_err
需要自定义 E？ → expected<T, E> + unexpected
```

---

## 6. 教程 C：and_then / transform 链式

```cpp
auto doubled = parse_positive("21").and_then([](int n) -> result<int> {
    if (n > 100) return result_err(std::errc::value_too_large);
    return result_ok(n * 2);
});

auto plus_one = result_ok(41).transform([](int n) { return n + 1; });
```

- `and_then`：下一步还可能失败 → 回调返回 `result`/`expected`。
- `transform`：只映射成功值类型。

---

## 7. 教程 D：or_else / transform_error 兜底

```cpp
auto r = result<int>(result_err(std::errc::io_error)).or_else(
    [](const std::error_code&) -> result<int> {
        return result_ok(0);  // 降级默认值
    });

expected<int, int> with_code(unexpect, 7);
auto as_string = std::move(with_code).transform_error(
    [](int e) { return std::string{"code="} + std::to_string(e); });
```

或更简单：`r.value_or(0)`。

---

## 8. 教程 E：自定义错误类型

```cpp
expected<std::string, std::string> load_name(bool ok_flag) {
    if (!ok_flag) return unexpected(std::string{"name missing"});
    return expected<std::string, std::string>(std::in_place, "alice");
}
```

`T`/`E` 易混时务必 `unexpected`。

---

## 9. 场景速查

| 我想… | 步骤 |
|------|------|
| 解析/校验返回 | A：`result_ok` / `result_err` + `if (r)` |
| 只表示成败无载荷 | B：`result<void>` |
| 多步流水线 | C：`and_then` / `transform` |
| 失败给默认 / 改错误形态 | D：`or_else` / `value_or` / `transform_error` |
| 非 `error_code` 错误 | E：`expected<T, E>` |
| 失败自动再试 | [retry.md](./retry.md) |

---

## 10. 和 retry / 异常怎么选

| 需求 | 选 |
|------|----|
| 单次调用的成功/失败 | **result / expected** |
| 瞬时失败再试几次 | retry（包一层返回 result 的 op） |
| 真正不可恢复 / 编程错误 | 异常（如 `bad_expected_access`、逻辑 bug） |

---

## 相关文件

- 实现：`reliability/result/expected.h`
- 示例：`demo/reliability/result/expected_demo.cpp`
- 测试：`tests/expected_test.cpp`
- 相关：[retry.md](./retry.md)、[time.md](./time.md)

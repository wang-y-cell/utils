# result / expected

用返回值表示成功或失败，避免把异常当作日常控制流。

| 项 | 说明 |
|----|------|
| 头文件 | [`component/result/expected.h`](../component/result/expected.h) |
| 命名空间 | `utils` |
| 伞头 | 已由 `#include "utils/utils.h"` 重导出 |
| 示例 | `demo/result/expected_demo.cpp`（`demo_expected`） |

---

## 目录

1. [快速入门](#1-快速入门)
2. [类型一览](#2-类型一览)
3. [expected](#3-expected)
4. [unexpected](#4-unexpected)
5. [工厂函数](#5-工厂函数)
6. [result 别名](#6-result-别名)
7. [单子式组合](#7-单子式组合)
8. [注意事项](#8-注意事项)

---

## 1. 快速入门

```cpp
#include "component/result/expected.h"
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

    auto doubled = parse_positive("21").and_then([](int n) -> result<int> {
        return result_ok(n * 2);
    });
}
```

---

## 2. 类型一览

| 类型 | 说明 |
|------|------|
| `expected<T, E>` | 要么持有成功值 `T`，要么持有错误 `E` |
| `expected<void, E>` | 无成功载荷的成功/失败 |
| `unexpected<E>` | 显式构造错误态，消歧义 |
| `result<T>` | `expected<T, std::error_code>` |
| `bad_expected_access` | 对空 `expected` 调 `value()` 时抛出 |
| `unexpect` | 标签，配合 `expected(unexpect, ...)` |

---

## 3. expected

```cpp
template <class T, class E>
class expected;

template <class E>
class expected<void, E>;
```

### 3.1 查询与访问

| API | 说明 |
|-----|------|
| `bool has_value() const` | 是否成功 |
| `explicit operator bool() const` | 同 `has_value()` |
| `T& value()` | 成功返回引用；失败抛 `bad_expected_access` |
| `E& error()` | 失败返回错误；成功态为编程错误（assert） |
| `T value_or(U&&)` | 失败时返回默认值 |
| `E error_or(G&&)` | 成功时返回默认错误 |
| `operator*` / `operator->` | 不检查，仅在已知有值时使用 |

推荐日常写法：`if (r) { use(*r); } else { use(r.error()); }`。

### 3.2 构造摘要

- 默认构造（`T` 可默认构造时）→ 成功空值 / 默认 `T`
- 由 `U&&` 构造成功值
- 由 `unexpected<E>` 构造失败
- `in_place` / `unexpect` 标签构造

`E` **不能**为 `void`。

### 3.3 void 特化

`expected<void, E>` / `result<void>`：成功无载荷；`and_then` / `transform` 的回调无参数。

```cpp
result<void> save() {
    if (!ok) return result_err(std::errc::io_error);
    return result_ok();
}
```

---

## 4. unexpected

显式标记「这是错误」，避免与成功值类型混淆。

```cpp
template <class E>
class unexpected;

unexpected(std::errc::invalid_argument);
auto u = unexpected(std::make_error_code(std::errc::timed_out));
E& error();
```

CTAD：`unexpected(E) -> unexpected<E>`。

---

## 5. 工厂函数

| 函数 | 含义 |
|------|------|
| `ok(T&&)` | `expected<decay_t<T>, error_code>` 成功 |
| `ok()` | `expected<void, error_code>` 成功 |
| `err(E&&)` | 得到 `unexpected` |
| `result_ok()` / `result_ok(T&&)` | `result` 成功 |
| `result_err(std::error_code)` | 失败（赋给 `result` 使用） |
| `result_err(std::errc)` | 同上 |

```cpp
result<int> a = result_ok(1);
result<int> b = result_err(std::errc::invalid_argument);
result<int> c = unexpected(std::make_error_code(std::errc::timed_out));
```

---

## 6. result 别名

```cpp
template <class T>
using result = expected<T, std::error_code>;
```

本仓库业务错误路径优先使用 `result` + `result_ok` / `result_err`，与 `retry`、`sql`、`invoke` 等一致。

---

## 7. 单子式组合

| 方法 | 说明 |
|------|------|
| `and_then(F)` | 成功则调用 `F`，返回新的 `expected` |
| `transform(F)` | 成功则映射值类型 |
| `or_else(F)` | 失败则调用 `F` |
| `transform_error(F)` | 映射错误类型 |

提供 `&` / `const&` / `&&` 重载。

```cpp
auto r = parse("21")
    .and_then([](int n) -> result<int> {
        if (n > 100) return result_err(std::errc::value_too_large);
        return result_ok(n * 2);
    })
    .or_else([](std::error_code ec) -> result<int> {
        return result_ok(-1);  // 降级默认值示例
    });
```

---

## 8. 注意事项

1. 业务失败请 `return result_err(...)` / `unexpected`，不要靠抛异常分支。
2. 优先 `if (r)` + `*r`，少用会抛异常的 `value()`。
3. 仅在确认失败时调用 `error()`。
4. `T` 与 `E` 易混淆时用 `unexpected` 消歧义。

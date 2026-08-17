# scope_guard

作用域结束时自动执行清理，覆盖提前 `return` 与异常路径。

| 项 | 说明 |
|----|------|
| 头文件 | [`component/scope_guard/scope_guard.h`](../component/scope_guard/scope_guard.h) |
| 命名空间 | `utils` |
| 伞头 | 已由 `utils/utils.h` 重导出 |
| 示例 | `demo/scope_guard/demo.cpp`（`demo_scope_guard`） |

---

## 目录

1. [快速入门](#1-快速入门)
2. [类型对比](#2-类型对比)
3. [scope_guard](#3-scope_guard)
4. [scope_success / scope_fail](#4-scope_success--scope_fail)
5. [UTILS_DEFER](#5-utils_defer)
6. [注意事项](#6-注意事项)

---

## 1. 快速入门

```cpp
#include "component/scope_guard/scope_guard.h"
using namespace utils;

void process(FILE* f) {
    auto guard = make_scope_guard([&] { fclose(f); });

    UTILS_DEFER { unlock_mutex(); };

    if (error) return;   // 仍会 fclose / unlock

    int old = flag;
    scope_fail rollback{[&] { flag = old; }};
    flag = 1;
    // 若后面抛异常 → 回滚；正常离开 → 不回滚
}
```

---

## 2. 类型对比

| 类型 | 析构时执行条件 |
|------|----------------|
| `scope_guard<F>` | 仍 active（任意离开作用域） |
| `scope_success<F>` | active 且**未**因异常离开（`uncaught_exceptions` 未增加） |
| `scope_fail<F>` | active 且因异常离开 |

工厂：`make_scope_guard` / `make_scope_success` / `make_scope_fail`。

---

## 3. scope_guard

```cpp
template <class F>
class scope_guard;
```

| API | 说明 |
|-----|------|
| `scope_guard(F&&)` / `(const F&)` | 显式构造 |
| 可移动，不可拷贝 | |
| `void dismiss() noexcept` | 取消，析构不再执行 |
| `void release() noexcept` | 同 `dismiss` |
| `bool active() const` | 是否仍会执行 |

```cpp
auto g = make_scope_guard([&] { resource.release(); });
if (transferred) g.dismiss();  // 所有权已移交，勿再 release
```

---

## 4. scope_success / scope_fail

API 与 `scope_guard` 类似（`dismiss` / `release`），差别仅在执行条件。

```cpp
scope_success commit{[&] { db.commit(); }};
scope_fail rollback{[&] { db.rollback(); }};

do_work();  // 成功离开 → 只 commit；抛异常 → 只 rollback
```

---

## 5. UTILS_DEFER

```cpp
UTILS_DEFER { unlock(); };
```

展开为一个不可 `dismiss` 的 `scope_guard`。需要取消时请改用 `make_scope_guard`。

---

## 6. 注意事项

1. 清理函数应**不抛异常**；栈展开中再抛会导致 `std::terminate`。
2. `scope_success` / `scope_fail` 依赖 `std::uncaught_exceptions()`，勿在析构路径上做复杂控制流。
3. 与 `result` 搭配：失败用返回值，资源用 guard，职责清晰。

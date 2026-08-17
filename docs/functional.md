# functional：function_ref / any_invocable

两套回调胶水类型，对应「参数里临时用」与「存起来可移动」。

| 项 | 说明 |
|----|------|
| 头文件 | [`function_ref.h`](../component/functional/function_ref.h)、[`any_invocable.h`](../component/functional/any_invocable.h) |
| 命名空间 | `utils` |
| 伞头 | 均已由 `utils/utils.h` 重导出 |
| 示例 | `demo/functional/demo.cpp`（`demo_functional`） |

---

## 目录

1. [选型](#1-选型)
2. [function_ref](#2-function_ref)
3. [any_invocable](#3-any_invocable)
4. [对比 std::function](#4-对比-stdfunction)
5. [示例](#5-示例)

---

## 1. 选型

| 类型 | 拥有目标？ | 可拷贝？ | 典型场景 |
|------|------------|----------|----------|
| `function_ref` | 否（只存地址） | 是 | 函数参数，用完即走 |
| `any_invocable` | 是（类型擦除） | 否（只移动） | 任务队列、成员、捕获 `unique_ptr` |
| `std::function` | 是 | 是（要求可拷贝） | 需要拷贝的回调 |

---

## 2. function_ref

非拥有的可调用引用，语义类似 `string_view`。

```cpp
template <class Sig>
class function_ref;  // R(Args...) / const / noexcept 特化
```

### API

| API | 说明 |
|-----|------|
| 默认 / `nullptr` | 空引用 |
| 从可调用对象 / 函数指针构造 | 不拥有，只观察 |
| `R operator()(Args...) const` | 空：assert（NDEBUG 下 UB） |
| `explicit operator bool() const` | 是否非空 |
| `swap` | 部分特化提供 |

### 注意

- **不延长**目标寿命；勿把局部 lambda 的 `function_ref` 存出作用域。
- 适合 API 边界：`void sort_by(function_ref<bool(const T&, const T&)>)`。

```cpp
void apply_each(const std::vector<int>& v, function_ref<void(int)> visitor) {
    for (int x : v) visitor(x);
}

void print(int x) { std::cout << x; }
apply_each({1, 2, 3}, print);
apply_each(v, [](int x) { std::cout << x * 2; });
```

---

## 3. any_invocable

可移动的类型擦除可调用包装（约 **32 字节 SBO**）。

```cpp
template <class Sig>
class any_invocable;
```

### API

| API | 说明 |
|-----|------|
| 默认 / `nullptr` | 空 |
| 仅可移动 | 不可拷贝 |
| 从可调用构造 | 可捕获 move-only |
| 可从 `std::function` 迁入 | 非 const 特化 |
| `reset()` / `= nullptr` | 清空 |
| `explicit operator bool()` | |
| `R operator()` | 空抛 `std::bad_function_call`（noexcept 特化用 assert） |
| `swap` | |

```cpp
any_invocable<int()> task = [p = std::make_unique<int>(40)] {
    return *p + 2;
};
int v = task();  // 42

any_invocable<void()> q = std::move(task);  // task 置空
```

---

## 4. 对比 std::function

| | `std::function` | `any_invocable` | `function_ref` |
|--|-----------------|-----------------|----------------|
| 拷贝 | 要 | 不要 | 拷贝的是引用 |
| move-only 捕获 | 不行 | 可以 | 不拥有 |
| 空调用 | 抛 `bad_function_call` | 同左（通常） | assert |
| 分配 | 可能堆 | SBO + 堆 | 无 |

---

## 5. 示例

```cpp
// 参数：用 function_ref
int fold(function_ref<int(int, int)> op, int a, int b) {
    return op(a, b);
}

// 存储：用 any_invocable
std::vector<any_invocable<void()>> jobs;
jobs.push_back([p = std::make_unique<Res>()] { p->run(); });
```

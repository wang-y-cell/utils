# deadline / stop_watch

截止时间与简单计时器。协作取消请直接使用标准库 `std::stop_token` / `std::stop_source`。

| 项 | 说明 |
|----|------|
| 头文件 | [`component/time/deadline.h`](../component/time/deadline.h) |
| 命名空间 | `utils` |
| 时钟 | `std::chrono::steady_clock` |
| 伞头 | 已由 `utils/utils.h` 重导出 |
| 示例 | `demo/time/demo.cpp`（`demo_deadline`） |

---

## 目录

1. [deadline](#1-deadline)
2. [stop_watch](#2-stop_watch)
3. [与 retry 组合](#3-与-retry-组合)
4. [示例](#4-示例)

---

## 1. deadline

表示一个绝对截止时刻。

### API

| API | 说明 |
|-----|------|
| `deadline()` | 等同 `never()`（永不过期） |
| `explicit deadline(time_point)` | |
| `static deadline after(duration)` | 从现在起经过 `duration` |
| `static deadline at(time_point)` | |
| `static deadline never()` | |
| `time_point time_point_value() const` | |
| `bool expired() const` | 是否已过期 |
| `duration remaining() const` | 剩余时间；已过期 → `zero` |
| `remaining_as<Dur = milliseconds>()` | 按指定单位取剩余 |

```cpp
using namespace std::chrono_literals;

auto d = deadline::after(200ms);
while (!d.expired()) {
    // 工作…
}
auto left = d.remaining();
```

---

## 2. stop_watch

构造即开始计时。

| API | 说明 |
|-----|------|
| `stop_watch()` | 开始 |
| `void reset()` | 重新从现在计时 |
| `duration elapsed() const` | |
| `elapsed_as<Dur = milliseconds>()` | |
| `std::int64_t elapsed_ms() const` | |
| `std::int64_t elapsed_us() const` | |

```cpp
stop_watch sw;
do_work();
std::cout << sw.elapsed_ms() << " ms\n";
```

---

## 3. 与 retry 组合

`retry(..., token, deadline)` 会在截止后停止重试，并把睡眠截断到 `remaining()`。详见 [retry.md](./retry.md)。

---

## 4. 示例

```cpp
auto d = deadline::after(40ms);
stop_watch sw;
while (!d.expired()) {
    // poll
}
std::cout << "loop took " << sw.elapsed_ms() << " ms, remaining="
          << d.remaining_as<std::chrono::milliseconds>().count() << "\n";
```

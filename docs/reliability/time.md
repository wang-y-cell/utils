# deadline

`reliability/time/deadline.h` 提供基于 `std::chrono::steady_clock` 的绝对
截止时间。协作取消直接使用标准库 `std::stop_token` / `std::stop_source`。

主要操作为 `after`、`at`、`never`、`expired`、`remaining` 和
`remaining_as`。

```cpp
using namespace std::chrono_literals;

auto d = deadline::after(200ms);
while (!d.expired()) {
    // 工作…
}
auto left = d.remaining();
```

`retry(..., token, deadline)` 会在截止后停止重试，并把等待截断到剩余时间。
详见 [retry.md](./retry.md)。

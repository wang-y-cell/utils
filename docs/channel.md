# channel

进程内线程间消息管道：有界/无界队列，偏**数据流**与背压（事件回调请用 [signal_and_slots](./signal_and_slots.md)）。

| 项 | 说明 |
|----|------|
| 头文件 | [`component/channel/channel.h`](../component/channel/channel.h) |
| 命名空间 | `utils` |
| 伞头 | 已由 `utils/utils.h` 重导出 |
| 示例 | `demo/channel/demo.cpp`（`demo_channel`） |

---

## 目录

1. [快速入门](#1-快速入门)
2. [构造](#2-构造)
3. [发送](#3-发送)
4. [接收](#4-接收)
5. [关闭与观测](#5-关闭与观测)
6. [注意事项](#6-注意事项)

---

## 1. 快速入门

```cpp
#include "component/channel/channel.h"
using namespace utils;

channel<int> ch(64);  // 容量 64；0 = 无界

ch.send(1);
auto v = ch.recv();   // optional<int>
if (v) std::cout << *v << "\n";

ch.close();
while (auto x = ch.recv()) {
    // 排空剩余
}
```

---

## 2. 构造

```cpp
template <class T>
class channel;  // 不可拷贝、不可移动

explicit channel(std::size_t capacity = 0);  // 0 = 无界
```

头文件约定模型为有界/无界 **MPSC**（多生产者单消费者风格用法；具体并发以实现为准，请按文档与 demo 使用）。

---

## 3. 发送

| API | 说明 |
|-----|------|
| `bool send(T value)` | 可阻塞等待空位；已关闭 → `false` |
| `bool try_send(T value)` | 非阻塞；满或已关闭 → `false` |

```cpp
if (!ch.try_send(42)) {
    // 背压或已关闭
}
ch.send(7);  // 有界满时阻塞直到有空位或关闭
```

---

## 4. 接收

| API | 说明 |
|-----|------|
| `std::optional<T> recv()` | 可阻塞；关闭且队列空 → `nullopt` |
| `std::optional<T> try_recv()` | 非阻塞；空 → `nullopt` |

---

## 5. 关闭与观测

| API | 说明 |
|-----|------|
| `void close()` | 关闭；之后 `send` 失败，`recv` 排空后结束 |
| `bool closed() const` | |
| `size_t size() const` | 当前队列长度 |
| `size_t capacity() const noexcept` | 容量（无界时语义以实现为准） |

---

## 6. 注意事项

1. `close` 后务必继续 `recv` 直到 `nullopt`，避免遗漏数据。
2. 信号槽适合「谁通知谁」；channel 适合「生产者 → 消费者」流水线。
3. 与 `thread_pool` / `executor` 组合：worker `send`，另一线程 `recv` 汇总。

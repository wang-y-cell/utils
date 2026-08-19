# channel 使用教程（`utils::channel`）

进程内线程间**消息管道**：有界/无界队列，偏数据流与背压。  
事件回调、对象亲和请用 [signal_and_slots](./signal_and_slots.md)；后台跑任务用 [thread_pool](./thread_pool.md)。

本文以**怎么用**为主：可行操作、异常 Tips、分场景教程。

| 项 | 说明 |
|----|------|
| 头文件 | [`concurrency/channel/channel.h`](../../concurrency/channel/channel.h) |
| 命名空间 | `utils` |
| 伞头 | 已由 `utils/utils.h` / `concurrency/concurrency.h` 重导出 |
| 可运行示例 | `demo/concurrency/channel/demo.cpp`（目标：`demo_channel`） |

```bash
cmake --build build --target demo_channel
./build/demo_channel   # Windows: build\demo_channel.exe
```

---

## 目录

1. [五分钟心智模型](#1-五分钟心智模型)
2. [全部可行操作一览](#2-全部可行操作一览)
3. [异常操作与 Tips（必读）](#3-异常操作与-tips必读)
4. [教程 A：最小收发](#4-教程-a最小收发)
5. [教程 B：有界背压](#5-教程-b有界背压)
6. [教程 C：多生产者 → 关通道 → 排空](#6-教程-c多生产者--关通道--排空)
7. [教程 D：和线程池组合](#7-教程-d和线程池组合)
8. [场景速查](#8-场景速查)
9. [和 signal / thread_pool 怎么选](#9-和-signal--thread_pool-怎么选)

---

## 1. 五分钟心智模型

```text
生产者线程(s)                    消费者线程
     │                                │
     │  send / try_send               │  recv / try_recv
     ├──────────►  queue_<T>  ◄────────┤
     │              │                 │
     │         close() 后             │
     │   send 失败；recv 排空后 nullopt │
```

三条硬规则：

1. **`capacity == 0` 无界**；`>0` 有界，满时 `send` 会堵、`try_send` 返回 false。
2. **`close` 后必须继续 `recv` 直到 `nullopt`**，否则队列里可能还有数据。
3. **适合「数据流」**；不适合「某个 object 上的回调」（那是信号槽）。

推荐入口：

| 你想… | 用 |
|------|----|
| 塞一条数据（可等空位） | `send` |
| 塞一条，满了立刻失败 | `try_send` |
| 取一条（可等数据） | `recv` |
| 取一条，空了立刻失败 | `try_recv` |
| 结束生产 | `close()`，再排空 |

---

## 2. 全部可行操作一览

### 2.1 创建

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 有界 | `channel<int> ch(64);` | 最多 64 个未取走的元素 |
| 无界 | `channel<int> ch;` 或 `ch(0)` | 只受内存限制 |
| 不可拷贝/移动 | — | 用引用/指针在线程间共享同一实例 |

### 2.2 发送 / 接收

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 阻塞发送 | `bool ok = ch.send(std::move(v));` | 满则等；已关闭 → `false` |
| 非阻塞发送 | `bool ok = ch.try_send(v);` | 满或已关闭 → `false` |
| 阻塞接收 | `auto v = ch.recv();` | `optional`；关闭且空 → `nullopt` |
| 非阻塞接收 | `auto v = ch.try_recv();` | 空 → `nullopt`（不论论是否关闭） |

### 2.3 关闭与观测

| 操作 | 怎么写 | 说明 |
|------|--------|------|
| 关闭 | `ch.close();` | 唤醒所有在等的生产者/消费者 |
| 是否已关 | `ch.closed()` | |
| 当前长度 | `ch.size()` | |
| 容量 | `ch.capacity()` | 构造时的值；`0` 表示无界 |

---

## 3. 异常操作与 Tips（必读）

### 3.1 禁止 / 易踩坑

| 异常操作 | 后果 | 正确做法 |
|----------|------|----------|
| `close` 后不再 `recv` | 队列残留数据被丢在语义上「没人读」 | `while (auto v = ch.recv()) { ... }` |
| 有界 channel + 多生产者，消费者很慢还狂 `send` | 生产者线程**长期阻塞** | `try_send` 背压，或加大容量/加快消费 |
| 把 channel 当信号槽 | 没有「接收者对象 / 亲和线程」 | 事件用 `signal_and_slots` |
| 多个消费者同时 `recv` 当「单消费者协议」 | 实现上有锁能跑，但消息会被分走，难推理 | 文档约定按 **MPSC（多产单消）** 来用 |
| `try_recv` 得到 `nullopt` 就以为关闭了 | 可能只是暂时空 | 要结束看 `closed()` 或阻塞 `recv` 排空 |

### 3.2 Tips

**Tip 1 — `send` 会不会阻塞？**

- 无界：一般不因「满」而堵（还会抢锁）。
- 有界且满：堵到有人 `recv`/`try_recv` 腾出空位，或 `close`。
- 已 `close`：立刻 `false`，不入队。

**Tip 2 — `recv` 什么时候返回 `nullopt`？**

- 仅当：**已关闭且队列已空**。
- 未关闭但空：阻塞等数据（`recv`）或立刻 `nullopt`（`try_recv`）。

**Tip 3 — `close` 可重复？**

- 再 `close` 仍是关闭状态；关键是关闭后排空。

**Tip 4 — 和线程池**

- worker 里 `send`，主线程或专职消费者 `recv`；不要假设 channel 自己会开线程。

---

## 4. 教程 A：最小收发

```cpp
#include "concurrency/channel/channel.h"
#include <iostream>

using namespace utils;

int main() {
    channel<int> ch(64);

    ch.send(1);
    if (auto v = ch.recv()) {
        std::cout << *v << "\n";
    }

    ch.close();
    while (auto x = ch.recv()) {
        // 排空（本例已空，直接结束）
    }
}
```

---

## 5. 教程 B：有界背压

```cpp
channel<int> ch(2);

ch.send(1);
ch.send(2);
bool third = ch.try_send(3);   // false：满了且不阻塞

auto a = ch.recv();            // 腾出一个空位
ch.send(3);                    // 现在可以再 send
```

决策：

```text
生产者不能被堵住？ → try_send
可以等消费者？     → send
```

---

## 6. 教程 C：多生产者 → 关通道 → 排空

```cpp
#include "concurrency/channel/channel.h"
#include <thread>

using namespace utils;

channel<int> pipe(0);  // 无界，避免 join 前堵死

std::thread p1([&] {
    for (int i = 0; i < 5; ++i) pipe.send(i);
});
std::thread p2([&] {
    for (int i = 100; i < 105; ++i) pipe.send(i);
});
p1.join();
p2.join();

pipe.close();   // 告诉消费者：不会再有新数据

int sum = 0;
while (auto v = pipe.recv()) {
    sum += *v;
}
```

顺序要点：

1. 生产者跑完（或确定不再生产）  
2. **`close()`**  
3. 消费者 `recv` 直到 `nullopt`

若先 `close` 再让生产者 `send`，那些 `send` 会失败。

---

## 7. 教程 D：和线程池组合

```cpp
#include "concurrency/channel/channel.h"
#include "concurrency/thread_pool/thread_pool.h"

thread_pool pool(4);
channel<int> results(128);

for (int i = 0; i < 100; ++i) {
    pool.add_task([&, i] {
        results.send(i * i);   // 多 worker 生产
    });
}
pool.wait();
results.close();

while (auto v = results.recv()) {
    // 单线程汇总
}
```

**Tip：** 有界 results + 很快的 `send` 可能反压线程池任务；需要时用 `try_send` 或更大容量。

---

## 8. 场景速查

| 我想… | 步骤 |
|------|------|
| 同进程传几个整数 | A：`send` / `recv` |
| 限流管道 | B：有界 + `try_send` |
| 多线程汇总结果 | C 或 D：多 `send` → `close` → 排空 |
| UI 事件回调 | 别用 channel → [signal_and_slots](./signal_and_slots.md) |
| 只想跑任务不要消息 | [thread_pool](./thread_pool.md) |

---

## 9. 和 signal / thread_pool 怎么选

| 需求 | 选 |
|------|----|
| 谁通知谁（对象、槽、线程亲和） | signal_and_slots |
| 生产者 → 消费者的数据队列 | **channel** |
| 一批可调用丢到后台跑 | thread_pool |
| 「在哪执行」可替换 | executor |

---

## 相关文件

- 实现：`concurrency/channel/channel.h`
- 示例：`demo/concurrency/channel/demo.cpp`
- 测试：`tests/channel_test.cpp`

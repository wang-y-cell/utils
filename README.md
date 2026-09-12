# utils

C++20 **header-only** 工具库，面向日常业务里的并发、错误处理与内存热点路径。无强制第三方运行时依赖（测试使用 vendored GoogleTest）。

三大主题：

| 目录 | 内容 |
|------|------|
| `concurrency/` | 信号槽与事件循环、线程池、Channel、Executor |
| `reliability/` | `result`返回值错误检查 / expected、Try 宏、重试、截止时间 |
| `memory/` | 无锁固定块池、对象池, 多档调节,高档可检查内存使用情况,内存泄漏以及泄漏位置,低档无检查但效率客观, 灵活配置内存配置选择适合自己的内存区间|

设计取向：接口小、可按主题单独 include；内存池默认无锁（调用方保证线程边界），并提供可关掉的泄漏 / 浪费诊断档。

```cpp
#include "utils/utils.h"                 // 三主题一并引入
// 或按需：
#include "concurrency/concurrency.h"
#include "reliability/reliability.h"
#include "memory/memory.h"
```

主题伞头：`concurrency/concurrency.h`、`reliability/reliability.h`、`memory/memory.h`。

---

## 文档

以下为**使用教程**（心智模型 / 可行操作 / Tips），不是纯 API 说明书。也可从 [docs/README.md](./docs/README.md) 进入。

### 并发与事件

- [安全的信号槽与事件循环](./docs/concurrency/signal_and_slots.md)
- [线程池](./docs/concurrency/thread_pool.md)
- [Channel](./docs/concurrency/channel.md)
- [Executor](./docs/concurrency/executor.md)

### 可靠性与错误

- [Result / Expected](./docs/reliability/result.md)
- [Retry](./docs/reliability/retry.md)
- [Deadline](./docs/reliability/time.md)

### 内存管理

- [Memory Pool / Allocator / Object Pool](./docs/memory/memory.md)

可运行示例见 [Demo 指南](./demo/README.md)。

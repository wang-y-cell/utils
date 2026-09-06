# utils 文档中心

总入口为 `#include "utils/utils.h"`。项目公开三个主题：

- `concurrency/`：signal_and_slots、thread_pool、channel、executor
- `reliability/`：result、retry、deadline
- `memory/`：memory_pool、memory_allocator、typed_alloc、object_pool

对应主题伞头为 `concurrency/concurrency.h`、
`reliability/reliability.h`、`memory/memory.h`。

以下文档均为**使用教程**（心智模型 / 可行操作 / 异常 Tips / 分场景步骤），不是纯 API 说明书。

## 并发与事件

- [信号槽与事件循环](./docs/concurrency/signal_and_slots.md)
- [线程池](./docs/concurrency/thread_pool.md)
- [Channel](./docs/concurrency/channel.md)
- [Executor](./docs/concurrency/executor.md)

## 可靠性与错误

- [Result / Expected](./docs/reliability/result.md)
- [Retry](./docs/reliability/retry.md)
- [Deadline](./docs/reliability/time.md)

## 内存管理

- [Memory Pool / Allocator / Object Pool](./docs/memory/memory.md)

可运行示例见 [Demo 指南](./docs/demo/README.md)。

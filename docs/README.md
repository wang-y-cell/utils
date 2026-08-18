# utils 文档中心

总入口为 `#include "utils/utils.h"`。项目只公开四个主题：

- `concurrency/`：signal_and_slots、thread_pool、channel、executor
- `reliability/`：result、retry、deadline
- `memory/`：memory_pool、object_pool
- `facade/`：log、sql、json

对应主题伞头为 `concurrency/concurrency.h`、
`reliability/reliability.h`、`memory/memory.h` 和 `facade/facade.h`。

## 并发与事件

- [信号槽与事件循环](./concurrency/signal_and_slots.md)
- [线程池](./concurrency/thread_pool.md)
- [Channel](./concurrency/channel.md)
- [Executor](./concurrency/executor.md)

## 可靠性与错误

- [Result / Expected](./reliability/result.md)
- [Retry](./reliability/retry.md)
- [Deadline](./reliability/time.md)

## 内存管理

- [Memory Pool / Object Pool](./memory/memory.md)

## 门面

- [Log](./facade/log.md)
- [SQL](./facade/sql.md)
- [JSON](./facade/json.md)

可运行示例见 [Demo 指南](../demo/README.md)。

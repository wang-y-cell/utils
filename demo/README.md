# utils Demo 指南

示例与公开 API 一样按 `concurrency`、`reliability`、`memory` 三个主题组织。

```bash
cmake -S . -B build
cmake --build build --target demos
```

可用目标：

- 并发：`demo_signal`、`demo_thread_pool`、`demo_channel`、`demo_executor`
- 可靠性：`demo_expected`、`demo_retry`、`demo_deadline`
- 内存：`demo_memory`

头文件总入口为 `utils/utils.h`。详细说明见
[文档中心](../docs/README.md)。

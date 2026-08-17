# utils 文档中心

本目录为 [`utils`](../) 库的 API 与使用说明（风格接近 Qt 官方文档：概念 → 类/API → 示例）。

头文件总入口：`#include "utils/utils.h"`  
（`thread_pool` / `signal_and_slots` / `executor/adapters.h` 体量或依赖较大，需按需单独包含。）

可运行示例见 [`demo/README.md`](../demo/README.md)。

---

## 分层

| 目录 | 含义 |
|------|------|
| [`component/`](../component/) | 功能组件：自给自足的实现与工具 |
| [`adapter/`](../adapter/) | 门面：窄接口 + 可换后端 |

依赖方向：**adapter → component**，反向禁止。

---

## Component 文档

| 文档 | 模块 | 说明 |
|------|------|------|
| [result.md](./result.md) | `result` / `expected` | 错误传递，少用异常当控制流 |
| [scope_guard.md](./scope_guard.md) | `scope_guard` | RAII 收尾 / DEFER |
| [functional.md](./functional.md) | `function_ref` / `any_invocable` | 非拥有引用 vs 可移动类型擦除回调 |
| [thread_pool.md](./thread_pool.md) | `thread_pool` | 任务线程池（需单独 include） |
| [signal_and_slots.md](./signal_and_slots.md) | 信号与槽 | Qt 风格信号槽 + 事件循环 |
| [channel.md](./channel.md) | `channel` | 进程内有界/无界消息管道 |
| [time.md](./time.md) | `deadline` / `stop_watch` | 截止时间与计时 |
| [retry.md](./retry.md) | `retry` / `retry_policy` | 重试与退避 |

## Adapter 文档

| 文档 | 模块 | 说明 |
|------|------|------|
| [config.md](./config.md) | `config_view` | 配置只读视图（map / env） |
| [log.md](./log.md) | `utils::log` | 日志门面 |
| [executor.md](./executor.md) | `executor` | 统一「在哪执行」 |
| [sql.md](./sql.md) | `utils::sql` | 关系库薄门面 |

## 规划

| 文档 | 说明 |
|------|------|
| [adapter-plan.md](./adapter-plan.md) | 门面规划、判定标准与后续方向 |

---

## 推荐组合

```text
config 读参数 → log 记录
→ thread_pool / executor 跑任务
→ stop_token + deadline + retry 控制失败与超时
→ channel / signal_and_slots 传数据与事件
→ scope_guard / result 管资源与错误
```

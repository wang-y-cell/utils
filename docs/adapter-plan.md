# 门面 / 适配器 计划清单

本仓库将代码分为两层：

| 目录 | 含义 |
|------|------|
| `adapter/` | 门面 + 可换后端：业务依赖窄接口，实现可接第三方库 |
| `component/` | 功能组件：自给自足的实现与工具，一般不包一层第三方 |

头文件总入口：`#include "utils/utils.h"`  
按需另含：`#include "adapter/executor/adapters.h"`、`#include "component/signal_and_slots/signal_and_slots.h"`

---

## 1. 现有分层（已落地）

### 适配器层 `adapter/`

| 模块 | 统一什么 | 内置 / 可扩展 |
|------|----------|----------------|
| `config` | 配置读取（`get_*` / `section`） | `map_config`、`env_config`；可写 JSON/YAML 适配器 |
| `log` | 日志（级别 + `info`/`error`…） | `stream_backend`、`null_backend`；可接 spdlog 等 |
| `executor` | 「在哪执行」（`post` / `try_post`） | `inline_executor`、`any_executor`；`adapters.h` 包 `thread_pool` / `event_loop` |
| `sql` | 关系库（`query`/`exec`/事务） | `memory_db`；可接 SOCI / libpqxx |

### 功能组件层 `component/`

| 模块 | 说明 |
|------|------|
| `result` / `expected` | 错误传递类型 |
| `scope_guard` | RAII 收尾 |
| `functional` | `function_ref` / `any_invocable` |
| `thread_pool` | 任务线程池（可被 executor 适配） |
| `signal_and_slots` | 信号槽 + `event_loop`（可被 executor 适配） |
| `channel` | 进程内消息管道 |
| `time` / `deadline` | 截止时间 |
| `retry` | 重试 / 退避策略 |

依赖方向：**adapter → component**（例如 `executor/adapters.h` 依赖 `thread_pool`、`signal_and_slots`），反向禁止。

---

## 2. 建议新增的门面（网上库多、接口不一）

按收益与接口可收窄程度排序。

### 高优先级

| 领域 | 典型第三方 | 门面宜统一 |
|------|------------|------------|
| 序列化 / Codec | nlohmann、RapidJSON、protobuf、msgpack | `encode`/`decode`（与 config 分离） |
| 指标 Metrics | Prometheus C++、OpenTelemetry | `counter`/`gauge`/`histogram` |
| 追踪 Tracing | OpenTelemetry、Jaeger | span 生命周期、属性、上下文传播 |
| 缓存 / KV | Redis++、memcached、本地 LRU | `get`/`set`/`del`、TTL |
| 对象存储 | AWS SDK、MinIO、本地 FS | `put`/`get`/`exists`/`delete` |
| 消息 Broker | Kafka、RabbitMQ、NATS | `publish`/`subscribe`、ack（跨进程；≠ `channel`） |

### 中优先级（边界宜窄）

| 领域 | 说明 |
|------|------|
| 时钟 / 时间源 | 可注入 fake clock，方便 `deadline`/`retry` 单测 |
| 随机数 / ID | UUID、雪花；测试可复现 |
| 加密 / Hash | OpenSSL / libsodium；仅暴露窄的 hash/hmac/encrypt |
| 虚拟文件系统 | `read_file`/`write_file`；单测用内存 FS |
| 取消令牌视图 | 对接 asio / gRPC context，对外仍偏向 `stop_token` 语义 |

### 不建议再套门面

- `expected` / `scope_guard` / `function_ref`：小工具或自研错误模型
- 完整 ORM / 完整 RPC 框架：门面会肥成第二框架；只做薄适配器
- 已有完整实现的 `thread_pool` / `signal_and_slots` / `channel`：用 `executor` 适配「执行位置」即可
- **网络（TCP/UDP/HTTP/WebSocket）**：直接用 Boost.Asio / Beast；适配成本接近直接用库，本库不做网络门面
- 数据库访问已落地为 `adapter/sql`（薄 `query`/`exec`/事务，非 ORM）

---

## 3. 判定是否值得做门面

对某一领域问三句，均为「是」再动手：

1. 是否经常换实现，或单测需要假实现？
2. 第三方 API 是否过重，业务不该直接依赖？
3. 能否用大约 ≤5 个核心方法覆盖 80% 用法？

失败路径建议继续走 `component/result` 的 `result` / `expected`，与现有胶水风格一致。

---

## 4. 推荐落地顺序（本仓库）

已落地：`sql`（及既有 `config` / `log` / `executor`）。网络门面已移除，生产网络请用 Asio/Beast。

后续可按痛点追加：

1. **codec**（配置仍用 `config_view`，消息编解码单独门面）
2. **kv_store / cache**（本地 map ↔ Redis）
3. **metrics**（与 `log` 同属可观测性）

每新增一个 `adapter/<name>/`：提供窄接口 + 至少一种内置/空实现（便于单测），第三方适配放同目录或 `adapter/<name>/xxx_backend.h`，**不要**把第三方类型泄漏进业务头文件。

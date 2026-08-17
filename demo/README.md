# utils 模块 Demo 指南

本目录为每个胶水模块提供**可运行示例**。统一编译：

```bash
cmake -S . -B build
cmake --build build --target demos   # 若未定义 demos，见下方各 target
```

或编译单个示例，例如：

```bash
cmake --build build --target demo_expected
./build/demo_expected
```

头文件总入口：`#include "utils/utils.h"`  
（`thread_pool` / `signal_and_slots` 体量较大，按需单独包含。）

源码分层：

| 目录 | 含义 |
|------|------|
| `adapter/` | 门面：`config` / `log` / `executor` / `sql` |
| `component/` | 功能组件：`expected`、`thread_pool`、`channel` 等 |
| `docs/adapter-plan.md` | 后续可新增门面的规划 |

---

## 模块一览

| Demo 目标 | 源文件 | 模块 | 层 |
|-----------|--------|------|----|
| `demo_expected` | `demo/result/expected_demo.cpp` | result / expected | component |
| `demo_scope_guard` | `demo/scope_guard/demo.cpp` | scope_guard | component |
| `demo_functional` | `demo/functional/demo.cpp` | function_ref / any_invocable | component |
| `demo_executor` | `demo/executor/demo.cpp` | executor | adapter |
| `demo_thread_pool` | `demo/thread_pool/demo.cpp` | thread_pool | component |
| `demo_signal` | `demo/signal_and_slots/sample.cpp` | 信号与槽 | component |
| `demo_deadline` | `demo/time/demo.cpp` | stop_watch / deadline | component |
| `demo_retry` | `demo/retry/demo.cpp` | Retry / Backoff | component |
| `demo_channel` | `demo/channel/demo.cpp` | channel | component |
| `demo_config` | `demo/config/demo.cpp` | config_view | adapter |
| `demo_log` | `demo/log/demo.cpp` | Logging facade | adapter |
| `demo_sql` | `demo/sql/demo.cpp` | 关系库门面 | adapter |

---

## 1. result / expected — 错误传递

**解决**：用返回值表示成功/失败，少用异常当控制流。

```cpp
#include "component/result/expected.h"
result<int> r = result_ok(42);
if (!r) { use(r.error()); }
else    { use(*r); }
auto x = parse().and_then([](int n) -> result<int> {
    return result_ok(n * 2);
});
```

- 失败：`return result_err(std::errc::...)` 或 `unexpected(e)`
- `T`/`E` 易混淆时用 `unexpected` 消歧义
- 详见 `demo/result/expected_demo.cpp`

---

## 2. scope_guard — RAII 收尾

**解决**：提前 `return` / 异常时仍执行清理。

```cpp
auto g = utils::make_scope_guard([&] { fclose(f); });
UTILS_DEFER { unlock(); };
utils::scope_fail rollback{[&] { flag = old; }};  // 仅异常时
utils::scope_success commit{[&] { save(); }};     // 仅成功离开时
g.dismiss();  // 取消清理（所有权已移交）
```

---

## 3. function_ref / any_invocable — 回调胶水

| 类型 | 拥有？ | 场景 |
|------|--------|------|
| `function_ref` | 否 | 函数参数，用完即走 |
| `any_invocable` | 是（只移动） | 存进队列/成员，可捕获 `unique_ptr` |

```cpp
void sort_by(utils::function_ref<bool(const T&, const T&)> cmp);
utils::any_invocable<void()> task = [p = std::make_unique<T>()] { p->run(); };
```

---

## 4. executor — 统一“在哪执行”

**解决**：业务只依赖 `post(f)`，运行时可换线程池 / event_loop / 同步执行。

```cpp
#include "adapter/executor/executor.h"
#include "adapter/executor/adapters.h"
utils::inline_executor sync;
utils::thread_pool pool(4);
auto ex = utils::make_executor(pool);
ex.post([] { ... });
utils::any_executor any = ex;  // 类型擦除
```

---

## 5. thread_pool — 任务线程池

```cpp
utils::thread_pool pool(4, /*max_queue=*/1024);
auto fut = pool.submit([] { return 1 + 1; });
pool.add_task([] { ... });
pool.wait();
pool.shutdown();
```

- `submit`：异常进入 `future`
- 有界队列提供背压；`try_add_task` 满则失败

---

## 6. signal_and_slots — 跨线程信号槽

**解决**：Qt 风格事件、对象线程亲和、Queued / BlockingQueued 投递。

```cpp
class window : public utils::object { ... };
connect(btn.on_clicked, &win, &window::on_update_ui);
connect(btn.on_clicked, &win, &window::on_update_ui,
        utils::connection_type::automatic, utils::unique_connection);
btn.block_signals(true);   // 成员信号需写成 signal{this}
btn.on_clicked.disconnect(&win);
```

注意：派生类析构建议 `invalidate()`；跨线程对象先 `stop` worker。`blocking_queued` 要求目标 loop 正在 `run()`（典型是 `worker_thread`），并且互相阻塞等待时可能死锁。

---

## 7. stop_watch / deadline — 计时与截止

```cpp
auto d = utils::deadline::after(200ms);
while (!d.expired()) { ... }
auto left = d.remaining();

utils::stop_watch sw;
...
std::cout << sw.elapsed_ms();
```

协作取消请直接用标准库 `std::stop_token` / `std::stop_source`。

---

## 8. Retry / Backoff — 重试策略

```cpp
std::stop_source source;
auto r = utils::retry(
    []() -> utils::result<int> { return fetch(); },
    utils::retry_policy::exponential(5, 10ms),
    source.get_token(), deadline);
// 或带谓词：仅部分错误可重试
```

只描述策略；真正的 RPC/IO 由 lambda 完成。取消参数类型为 `std::stop_token`。

---

## 9. channel — 线程间数据管道

```cpp
utils::channel<int> ch(64);  // 0 = 无界
ch.send(1);
auto v = ch.recv();  // optional；close 且排空后 nullopt
ch.close();
```

信号槽偏回调；channel 偏数据流 / 背压。

---

## 10. config_view — 配置视图

```cpp
utils::map_config cfg;
cfg.set("port", std::int64_t{8080});
cfg.set("db.host", "localhost");
cfg.set_bool("debug", true);  // 勿用 set(key, bool) 与字符串重载混淆
auto db = cfg.section("db");
utils::env_config env("MYAPP_");  // MYAPP_PORT
```

不绑 JSON 库；可自行写 `from_nlohmann` 适配器。

---

## 11. Logging facade — 日志门面

```cpp
utils::log::set_backend(std::make_shared<utils::log::stream_backend>());
utils::log::set_level(utils::log::level::info);
utils::log::info("listen port={}", 8080);
// 测试: null_backend
```

业务不直接依赖 spdlog；换后端只改 `set_backend`。

---

## 12. sql — 关系库门面

```cpp
#include "adapter/sql/sql.h"
utils::sql::memory_db db;
db.on_query("SELECT 1", {{"n"}, {{"1"}}});
auto conn = db.open();
auto rows = conn.value()->query("SELECT 1");
conn.value()->begin();
conn.value()->commit();
```

同一访问模型（SQL + 事务）共用此门面；Redis 等 KV 不要塞进来。真库实现 `sql::connection` 即可。

网络（TCP/HTTP/WS）请直接使用 Boost.Asio / Beast，本库不做网络门面。

---

## 推荐组合

```text
Config 读参数 → log 记录
→ thread_pool / executor 跑任务
→ std::stop_token + deadline + Retry 控制失败与超时
→ channel / signal 传递结果与事件
→ scope_guard / expected 管资源与错误
```

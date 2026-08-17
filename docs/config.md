# config_view

配置**只读视图**门面：统一 `get_*` / `section`，不绑定 JSON/YAML 解析库。

| 项 | 说明 |
|----|------|
| 头文件 | [`adapter/config/config_view.h`](../adapter/config/config_view.h) |
| 命名空间 | `utils` |
| 伞头 | 已由 `utils/utils.h` 重导出 |
| 示例 | `demo/config/demo.cpp`（`demo_config`） |

---

## 目录

1. [快速入门](#1-快速入门)
2. [config_view](#2-config_view)
3. [map_config](#3-map_config)
4. [env_config](#4-env_config)
5. [section 语义](#5-section-语义)
6. [扩展](#6-扩展)

---

## 1. 快速入门

```cpp
#include "adapter/config/config_view.h"
using namespace utils;

map_config cfg;
cfg.set("port", static_cast<std::int64_t>(8080));
cfg.set("db.host", "localhost");
cfg.set_bool("debug", true);  // 勿用 set(key, bool) 与字符串重载混淆

auto port = cfg.get_int("port");           // optional<int64_t>
auto db = cfg.section("db");
auto host = db->get_string("host");        // "localhost"

env_config env("MYAPP_");  // 读 MYAPP_PORT 等
```

---

## 2. config_view

抽象基类。

```cpp
class config_view {
public:
    virtual ~config_view() = default;

    virtual std::optional<std::string> get_string(std::string_view key) const = 0;
    virtual std::optional<std::int64_t> get_int(std::string_view key) const;
    virtual std::optional<bool> get_bool(std::string_view key) const;
    virtual std::optional<double> get_double(std::string_view key) const;
    virtual std::shared_ptr<config_view> section(std::string_view name) const = 0;
};
```

| 方法 | 默认行为 |
|------|----------|
| `get_string` | 纯虚 |
| `get_int` | `get_string` + `stoll` |
| `get_bool` | 识别 `1/true/yes/on` 等（大小写不敏感实现以头文件为准） |
| `get_double` | 经字符串解析 |
| `section` | 纯虚；返回带前缀的子视图 |

---

## 3. map_config

内存字典实现，适合测试与手写配置。

| API | 说明 |
|-----|------|
| `void set(string key, string / const char* / int64_t / double)` | 写入 |
| `void set_bool(string key, bool)` | 专用，避免与 `const char*` 重载混淆 |
| `get_string` / `section` | 覆盖基类 |

```cpp
map_config cfg;
cfg.set("name", "demo");
cfg.set("timeout_ms", static_cast<std::int64_t>(3000));
cfg.set_bool("verbose", false);
```

---

## 4. env_config

从环境变量读取。

```cpp
explicit env_config(std::string prefix = {});
```

键变换规则（摘要）：

- 实际环境变量名 = `prefix` + key
- key 中 `.` → `_`
- 小写 → 大写

```cpp
env_config env("MYAPP_");
// get_string("port") → 读 MYAPP_PORT
// section("db")->get_string("host") → 读 MYAPP_DB_HOST
```

---

## 5. section 语义

`section("db")` 之后，子视图的 key 会自动加上 `"db."` 前缀（或 env 下对应前缀规则）。

```cpp
cfg.set("db.host", "127.0.0.1");
cfg.set("db.port", static_cast<std::int64_t>(5432));
auto db = cfg.section("db");
db->get_string("host");  // 等价读 "db.host"
```

---

## 6. 扩展

自行实现 `config_view`，例如从 nlohmann::json 适配：

```cpp
class json_config : public config_view {
    // get_string / section ...
};
```

业务代码只依赖 `config_view&`，便于单测注入 `map_config`。

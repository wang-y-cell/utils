# Logging facade（utils::log）

可插拔日志门面：业务不直接依赖 spdlog 等第三方。

| 项 | 说明 |
|----|------|
| 头文件 | [`adapter/log/log.h`](../adapter/log/log.h) |
| 命名空间 | `utils::log` |
| 伞头 | 已由 `utils/utils.h` 重导出 |
| 示例 | `demo/log/demo.cpp`（`demo_log`） |

---

## 目录

1. [快速入门](#1-快速入门)
2. [级别](#2-级别)
3. [backend](#3-backend)
4. [全局 API](#4-全局-api)
5. [内置后端](#5-内置后端)
6. [扩展](#6-扩展)

---

## 1. 快速入门

```cpp
#include "adapter/log/log.h"

utils::log::set_level(utils::log::level::debug);
utils::log::info("server listen port={}", 8080);
utils::log::error("open failed: {}", ec.message());

// 测试静默
utils::log::set_backend(std::make_shared<utils::log::null_backend>());
```

默认已是 `stream_backend`（通常写到 `cerr`），默认级别 `info`。

---

## 2. 级别

```cpp
enum class level {
    trace, debug, info, warn, error, off
};
```

低于当前级别或为 `off` 的日志会被直接跳过（不格式化）。

---

## 3. backend

```cpp
class backend {
public:
    virtual ~backend() = default;
    virtual void log(level lv, std::string_view msg) = 0;
};
```

---

## 4. 全局 API

| API | 说明 |
|-----|------|
| `void set_backend(shared_ptr<backend>)` | `nullptr` → 退回 `null_backend` |
| `void set_level(level)` | |
| `level get_level()` | |
| `void write(level, string_view)` | 原始写入 |
| `void log(level, format_string, Args&&...)` | `std::format` 风格 |
| `trace` / `debug` / `info` / `warn` / `error` | 带级别的格式化快捷方式 |

```cpp
log::log(log::level::warn, "retry {}/{}", i, n);
log::warn("retry {}/{}", i, n);  // 等价快捷
```

---

## 5. 内置后端

| 类型 | 说明 |
|------|------|
| `null_backend` | 丢弃全部输出（单测友好） |
| `stream_backend` | 输出到流（默认 `cerr`），带 mutex |

---

## 6. 扩展

实现 `backend` 即可对接 spdlog 等：

```cpp
class spdlog_backend : public utils::log::backend {
public:
    void log(level lv, std::string_view msg) override {
        // 映射到 spdlog::...
    }
};

log::set_backend(std::make_shared<spdlog_backend>());
```

业务头文件只依赖 `utils::log`，第三方类型不泄漏。

#pragma once

/**
 * Logging facade — 可替换后端（C++20）
 *
 *   utils::log::set_backend(std::make_shared<utils::log::StreamBackend>());
 *   utils::log::set_level(utils::log::Level::Info);
 *   utils::log::info("port={}", 8080);
 */

#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace utils::log {

enum class Level {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void log(Level level, std::string_view message) = 0;
};

class NullBackend : public Backend {
public:
    void log(Level, std::string_view) override {}
};

class StreamBackend : public Backend {
public:
    explicit StreamBackend(std::ostream& os = std::cerr) : os_(&os) {}

    void log(Level level, std::string_view message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        *os_ << '[' << level_name(level) << "] " << message << '\n';
    }

private:
    static const char* level_name(Level level) {
        switch (level) {
            case Level::Trace:
                return "TRACE";
            case Level::Debug:
                return "DEBUG";
            case Level::Info:
                return "INFO";
            case Level::Warn:
                return "WARN";
            case Level::Error:
                return "ERROR";
            default:
                return "OFF";
        }
    }

    std::ostream* os_;
    std::mutex mutex_;
};

namespace detail {

inline std::shared_ptr<Backend>& backend_slot() {
    static std::shared_ptr<Backend> b = std::make_shared<StreamBackend>();
    return b;
}

inline Level& level_slot() {
    static Level lv = Level::Info;
    return lv;
}

inline std::mutex& meta_mutex() {
    static std::mutex m;
    return m;
}

}  // namespace detail

inline void set_backend(std::shared_ptr<Backend> backend) {
    std::lock_guard<std::mutex> lock(detail::meta_mutex());
    detail::backend_slot() =
        backend ? std::move(backend) : std::make_shared<NullBackend>();
}

inline void set_level(Level level) {
    std::lock_guard<std::mutex> lock(detail::meta_mutex());
    detail::level_slot() = level;
}

[[nodiscard]] inline Level level() {
    std::lock_guard<std::mutex> lock(detail::meta_mutex());
    return detail::level_slot();
}

inline void write(Level lv, std::string_view message) {
    std::shared_ptr<Backend> b;
    Level min_lv;
    {
        std::lock_guard<std::mutex> lock(detail::meta_mutex());
        min_lv = detail::level_slot();
        b = detail::backend_slot();
    }
    if (!b || lv < min_lv || min_lv == Level::Off) {
        return;
    }
    b->log(lv, message);
}

template <class... Args>
void log(Level lv, std::format_string<Args...> fmt, Args&&... args) {
    if (lv < level() || level() == Level::Off) {
        return;
    }
    write(lv, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Trace, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Warn, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    log(Level::Error, fmt, std::forward<Args>(args)...);
}

}  // namespace utils::log

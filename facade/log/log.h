#pragma once

/**
 * Logging facade — 可替换后端（C++20）
 */

#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace utils::log {

enum class level {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    off = 5
};

class backend {
public:
    virtual ~backend() = default;
    virtual void log(level lv, std::string_view message) = 0;
};

class null_backend : public backend {
public:
    void log(level, std::string_view) override {}
};

class stream_backend : public backend {
public:
    explicit stream_backend(std::ostream& os = std::cerr) : os_(&os) {}

    void log(level lv, std::string_view message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        *os_ << '[' << level_name(lv) << "] " << message << '\n';
    }

private:
    static const char* level_name(level lv) {
        switch (lv) {
            case level::trace:
                return "TRACE";
            case level::debug:
                return "DEBUG";
            case level::info:
                return "INFO";
            case level::warn:
                return "WARN";
            case level::error:
                return "ERROR";
            default:
                return "OFF";
        }
    }

    std::ostream* os_;
    std::mutex mutex_;
};

namespace detail {

inline std::shared_ptr<backend>& backend_slot() {
    static std::shared_ptr<backend> b = std::make_shared<stream_backend>();
    return b;
}

inline level& level_slot() {
    static level lv = level::info;
    return lv;
}

inline std::mutex& meta_mutex() {
    static std::mutex m;
    return m;
}

}  // namespace detail

inline void set_backend(std::shared_ptr<backend> b) {
    std::lock_guard<std::mutex> lock(detail::meta_mutex());
    detail::backend_slot() =
        b ? std::move(b) : std::make_shared<null_backend>();
}

inline void set_level(level lv) {
    std::lock_guard<std::mutex> lock(detail::meta_mutex());
    detail::level_slot() = lv;
}

[[nodiscard]] inline level get_level() {
    std::lock_guard<std::mutex> lock(detail::meta_mutex());
    return detail::level_slot();
}

inline void write(level lv, std::string_view message) {
    std::shared_ptr<backend> b;
    level min_lv;
    {
        std::lock_guard<std::mutex> lock(detail::meta_mutex());
        min_lv = detail::level_slot();
        b = detail::backend_slot();
    }
    if (!b || lv < min_lv || min_lv == level::off) {
        return;
    }
    b->log(lv, message);
}

template <class... Args>
void log(level lv, std::format_string<Args...> fmt, Args&&... args) {
    const level min_lv = get_level();
    if (lv < min_lv || min_lv == level::off) {
        return;
    }
    write(lv, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    log(level::trace, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    log(level::debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    log(level::info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    log(level::warn, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    log(level::error, fmt, std::forward<Args>(args)...);
}

}  // namespace utils::log

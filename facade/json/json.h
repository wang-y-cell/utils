#pragma once

#include "facade/json/backend.h"
#include "facade/json/simple_backend.h"
#include "facade/json/value.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace utils::json {
namespace detail {

inline std::mutex& backend_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::shared_ptr<backend>& backend_slot() {
    static std::shared_ptr<backend> instance =
        std::make_shared<simple_backend>();
    return instance;
}

inline std::shared_ptr<backend> current_backend() {
    std::lock_guard<std::mutex> lock(backend_mutex());
    return backend_slot();
}

}  // namespace detail

inline void set_backend(std::shared_ptr<backend> replacement) {
    if (!replacement) {
        replacement = std::make_shared<simple_backend>();
    }
    std::lock_guard<std::mutex> lock(detail::backend_mutex());
    detail::backend_slot() = std::move(replacement);
}

inline void reset_backend() {
    set_backend(std::make_shared<simple_backend>());
}

[[nodiscard]] inline result<value> parse(std::string_view text) {
    try {
        return detail::current_backend()->parse(text);
    } catch (...) {
        return result_err(std::errc::invalid_argument);
    }
}

[[nodiscard]] inline result<std::string> stringify(const value& input,
                                                   bool pretty = false) {
    try {
        return detail::current_backend()->stringify(input, pretty);
    } catch (...) {
        return result_err(std::errc::invalid_argument);
    }
}

}  // namespace utils::json

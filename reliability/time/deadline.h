#pragma once

/**
 * deadline — 截止时间（C++20）
 *
 *   auto d = utils::deadline::after(200ms);
 *   while (!d.expired()) { ... }
 *   auto left = d.remaining();
 */

#include <chrono>

namespace utils {

class deadline {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using duration = clock::duration;

    deadline() noexcept : tp_(time_point::max()) {}

    explicit deadline(time_point tp) noexcept : tp_(tp) {}

    template <class Rep, class Period>
    static deadline after(std::chrono::duration<Rep, Period> d) {
        return deadline(clock::now() +
                        std::chrono::duration_cast<duration>(d));
    }

    static deadline at(time_point tp) noexcept { return deadline(tp); }

    static deadline never() noexcept { return deadline{}; }

    [[nodiscard]] time_point time_point_value() const noexcept { return tp_; }

    [[nodiscard]] bool expired() const noexcept {
        return clock::now() >= tp_;
    }

    [[nodiscard]] duration remaining() const noexcept {
        const auto now = clock::now();
        if (now >= tp_) {
            return duration::zero();
        }
        return tp_ - now;
    }

    template <class Dur = std::chrono::milliseconds>
    [[nodiscard]] Dur remaining_as() const noexcept {
        return std::chrono::duration_cast<Dur>(remaining());
    }

private:
    time_point tp_;
};

}  // namespace utils

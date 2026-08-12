#pragma once

/**
 * StopWatch / Deadline — 计时与截止时间（C++20）
 *
 *   auto d = utils::Deadline::after(200ms);
 *   while (!d.expired()) { ... }
 *   auto left = d.remaining();
 *
 *   utils::StopWatch sw;
 *   ...
 *   auto ms = sw.elapsed_ms();
 */

#include <chrono>
#include <cstdint>

namespace utils {

class Deadline {
public:
    using Clock = std::chrono::steady_clock;
    using time_point = Clock::time_point;
    using duration = Clock::duration;

    Deadline() noexcept : tp_(time_point::max()) {}

    explicit Deadline(time_point tp) noexcept : tp_(tp) {}

    template <class Rep, class Period>
    static Deadline after(std::chrono::duration<Rep, Period> d) {
        return Deadline(Clock::now() +
                        std::chrono::duration_cast<duration>(d));
    }

    static Deadline at(time_point tp) noexcept { return Deadline(tp); }

    static Deadline never() noexcept { return Deadline{}; }

    [[nodiscard]] time_point time_point_value() const noexcept { return tp_; }

    [[nodiscard]] bool expired() const noexcept {
        return Clock::now() >= tp_;
    }

    [[nodiscard]] duration remaining() const noexcept {
        const auto now = Clock::now();
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

class StopWatch {
public:
    using Clock = std::chrono::steady_clock;
    using duration = Clock::duration;

    StopWatch() noexcept : start_(Clock::now()) {}

    void reset() noexcept { start_ = Clock::now(); }

    [[nodiscard]] duration elapsed() const noexcept {
        return Clock::now() - start_;
    }

    template <class Dur = std::chrono::milliseconds>
    [[nodiscard]] Dur elapsed_as() const noexcept {
        return std::chrono::duration_cast<Dur>(elapsed());
    }

    [[nodiscard]] std::int64_t elapsed_ms() const noexcept {
        return elapsed_as<std::chrono::milliseconds>().count();
    }

    [[nodiscard]] std::int64_t elapsed_us() const noexcept {
        return elapsed_as<std::chrono::microseconds>().count();
    }

private:
    Clock::time_point start_;
};

}  // namespace utils

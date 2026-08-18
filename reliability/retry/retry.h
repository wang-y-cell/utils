#pragma once

/**
 * Retry / Backoff — 重试策略对象（C++20）
 *
 *   auto r = utils::retry(
 *       []() -> utils::result<int> { return fetch(); },
 *       utils::retry_policy::exponential(5, 10ms));
 *
 * 取消使用标准库 std::stop_token（可选参数）。
 */

#include "reliability/result/expected.h"
#include "reliability/time/deadline.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace utils {

class retry_policy {
public:
    using duration = std::chrono::steady_clock::duration;

    std::size_t max_attempts = 3;
    duration initial_delay = std::chrono::milliseconds(50);
    double multiplier = 2.0;
    duration max_delay = std::chrono::seconds(5);
    bool jitter = true;

    static retry_policy fixed(std::size_t attempts, duration delay) {
        retry_policy p;
        p.max_attempts = attempts == 0 ? 1 : attempts;
        p.initial_delay = delay;
        p.multiplier = 1.0;
        p.max_delay = delay;
        p.jitter = false;
        return p;
    }

    static retry_policy exponential(std::size_t attempts, duration initial,
                                   double mult = 2.0,
                                   duration max_d = std::chrono::seconds(5)) {
        retry_policy p;
        p.max_attempts = attempts == 0 ? 1 : attempts;
        p.initial_delay = initial;
        p.multiplier = mult < 1.0 ? 1.0 : mult;
        p.max_delay = max_d;
        p.jitter = true;
        return p;
    }

    /** failure_index：第几次失败（从 0 起）；耗尽返回 nullopt */
    [[nodiscard]] std::optional<duration> delay_after_failure(
        std::size_t failure_index) const {
        if (failure_index + 1 >= max_attempts) {
            return std::nullopt;
        }
        double scale = 1.0;
        for (std::size_t i = 0; i < failure_index; ++i) {
            scale *= multiplier;
        }
        const auto base_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(initial_delay)
                .count();
        auto raw = std::chrono::duration_cast<duration>(
            std::chrono::nanoseconds(
                static_cast<std::int64_t>(static_cast<double>(base_ns) * scale)));
        if (raw > max_delay) {
            raw = max_delay;
        }
        if (!jitter || raw.count() <= 0) {
            return raw;
        }
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<std::int64_t> dist(0, raw.count());
        return duration(dist(rng));
    }
};

namespace detail {

template <class R>
struct is_expected_result : std::false_type {};
template <class T, class E>
struct is_expected_result<expected<T, E>> : std::true_type {};

template <class R>
R make_cancel_result() {
    using E = typename R::error_type;
    if constexpr (std::is_same_v<E, std::error_code>) {
        return R(unexpect, std::make_error_code(std::errc::operation_canceled));
    } else if constexpr (std::is_constructible_v<E, std::errc>) {
        return R(unexpect, E{std::errc::operation_canceled});
    } else {
        static_assert(sizeof(R) == 0,
                      "retry cancel requires error_type constructible from "
                      "error_code/errc, or provide a prior failure result");
    }
}

}  // namespace detail

/**
 * 对返回 expected/result 的操作重试。
 * should_retry(error) 为 true 时才退避再试。
 */
template <class F, class Pred>
auto retry(F&& op, retry_policy policy, Pred&& should_retry,
           std::stop_token token = {},
           deadline deadline = deadline::never())
    -> std::invoke_result_t<F&> {
    using R = std::invoke_result_t<F&>;
    static_assert(detail::is_expected_result<R>::value,
                  "retry() requires expected/result return type");

    std::optional<R> last_fail;
    std::size_t attempt = 0;

    for (;;) {
        if (token.stop_requested() || deadline.expired()) {
            if (last_fail) {
                return std::move(*last_fail);
            }
            return detail::make_cancel_result<R>();
        }

        R result = op();
        if (result.has_value()) {
            return result;
        }

        if (!should_retry(result.error())) {
            return result;
        }

        auto delay = policy.delay_after_failure(attempt);
        if (!delay) {
            return result;
        }

        last_fail = std::move(result);

        auto sleep_for = *delay;
        const auto remain = deadline.remaining();
        if (remain < sleep_for) {
            sleep_for = remain;
        }
        if (sleep_for.count() > 0) {
            std::this_thread::sleep_for(sleep_for);
        }
        ++attempt;
    }
}

template <class F>
auto retry(F&& op, retry_policy policy, std::stop_token token = {},
           deadline deadline = deadline::never()) {
    return retry(
        std::forward<F>(op), std::move(policy),
        [](const auto&) { return true; }, std::move(token), deadline);
}

}  // namespace utils

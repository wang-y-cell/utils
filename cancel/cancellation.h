#pragma once

/**
 * Cancellation — 协作取消（C++20）
 *
 * 基于 std::stop_token / stop_source；提供成对工厂与便捷别名。
 *
 *   auto [token, source] = utils::make_cancellation();
 *   pool.submit([token] {
 *     while (!token.stop_requested()) { ... }
 *   });
 *   source.request_stop();
 */

#include <stop_token>
#include <utility>

namespace utils {

using StopToken = std::stop_token;
using StopSource = std::stop_source;

template <class Callback>
using StopCallback = std::stop_callback<Callback>;

/** 可拷贝的取消令牌视图（与 std::stop_token 相同语义） */
using CancellationToken = StopToken;

/** 取消源：request_stop 后所有关联 token 可见 */
class CancellationSource {
public:
    CancellationSource() = default;
    explicit CancellationSource(std::nostopstate_t) noexcept
        : source_(std::nostopstate) {}

    [[nodiscard]] CancellationToken token() const noexcept {
        return source_.get_token();
    }

    void request_stop() noexcept { source_.request_stop(); }

    [[nodiscard]] bool stop_requested() const noexcept {
        return source_.stop_requested();
    }

    [[nodiscard]] bool stop_possible() const noexcept {
        return source_.stop_possible();
    }

    [[nodiscard]] StopSource& native() noexcept { return source_; }
    [[nodiscard]] const StopSource& native() const noexcept { return source_; }

private:
    StopSource source_{};
};

struct CancellationPair {
    CancellationToken token;
    CancellationSource source;
};

[[nodiscard]] inline CancellationPair make_cancellation() {
    CancellationSource source;
    auto token = source.token();
    return CancellationPair{std::move(token), std::move(source)};
}

[[nodiscard]] inline bool stop_requested(const CancellationToken& token) noexcept {
    return token.stop_requested();
}

}  // namespace utils

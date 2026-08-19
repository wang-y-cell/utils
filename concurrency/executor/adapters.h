#pragma once

/**
 * executor 适配器 — thread_pool_executor / event_loop_executor
 *
 * 不拥有后端；move-only 可调用经 shared_ptr 包装以适配 std::function 任务队列。
 */

#include "concurrency/executor/executor.h"

#include "concurrency/signal_and_slots/signal_and_slots.h"
#include "concurrency/thread_pool/thread_pool.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace utils {
namespace detail {

template <class Sink, class F>
void post_copyable_or_shared(Sink&& sink, F&& f) {
    using FD = std::decay_t<F>;
    if constexpr (std::is_copy_constructible_v<FD>) {
        std::forward<Sink>(sink)(std::forward<F>(f));
    } else {
        auto shared = std::make_shared<FD>(std::forward<F>(f));
        std::forward<Sink>(sink)([shared] { (*shared)(); });
    }
}

}  // namespace detail

/**
 * 包装 thread_pool（非拥有）。
 * post → add_task（已停止时抛）；try_post → try_add_task。
 */
class thread_pool_executor {
public:
    explicit thread_pool_executor(thread_pool& pool) noexcept : pool_(&pool) {}

    template <class F>
    void post(F&& f) {
        detail::post_copyable_or_shared(
            [this](auto&& task) {
                pool_->add_task(std::forward<decltype(task)>(task));
            },
            std::forward<F>(f));
    }

    template <class F>
    bool try_post(F&& f) {
        bool ok = false;
        detail::post_copyable_or_shared(
            [this, &ok](auto&& task) {
                ok = pool_->try_add_task(std::forward<decltype(task)>(task));
            },
            std::forward<F>(f));
        return ok;
    }

    [[nodiscard]] thread_pool* target() const noexcept { return pool_; }

private:
    thread_pool* pool_;
};

/**
 * 包装 event_loop（非拥有）。
 * 未 running 时 post 与 event_loop 一致：静默丢弃。
 * try_post：未 running 返回 false，否则 post 并返回 true。
 */
class event_loop_executor {
public:
    explicit event_loop_executor(event_loop& loop) noexcept : loop_(&loop) {}

    template <class F>
    void post(F&& f) {
        detail::post_copyable_or_shared(
            [this](auto&& task) {
                loop_->post(std::forward<decltype(task)>(task));
            },
            std::forward<F>(f));
    }

    template <class F>
    bool try_post(F&& f) {
        bool ok = false;
        detail::post_copyable_or_shared(
            [this, &ok](auto&& task) {
                ok = loop_->post(std::forward<decltype(task)>(task));
            },
            std::forward<F>(f));
        return ok;
    }

    [[nodiscard]] event_loop* target() const noexcept { return loop_; }

private:
    event_loop* loop_;
};

[[nodiscard]] inline thread_pool_executor make_executor(thread_pool& pool) noexcept {
    return thread_pool_executor(pool);
}

[[nodiscard]] inline event_loop_executor make_executor(event_loop& loop) noexcept {
    return event_loop_executor(loop);
}

}  // namespace utils

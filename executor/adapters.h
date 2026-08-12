#pragma once

/**
 * Executor 适配器 — ThreadPoolExecutor / EventLoopExecutor
 *
 * 不拥有后端；move-only 可调用经 shared_ptr 包装以适配 std::function 任务队列。
 */

#include "executor/executor.h"

#include "signal_and_slots/signal_and_slots.h"
#include "thread_pool/thread_pool.h"

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
class ThreadPoolExecutor {
public:
    explicit ThreadPoolExecutor(thread_pool& pool) noexcept : pool_(&pool) {}

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
 * 包装 EventLoop（非拥有）。
 * 未 running 时 post 与 EventLoop 一致：静默丢弃。
 * try_post：未 running 返回 false，否则 post 并返回 true。
 */
class EventLoopExecutor {
public:
    explicit EventLoopExecutor(EventLoop& loop) noexcept : loop_(&loop) {}

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
        if (!loop_->isRunning()) {
            return false;
        }
        post(std::forward<F>(f));
        return true;
    }

    [[nodiscard]] EventLoop* target() const noexcept { return loop_; }

private:
    EventLoop* loop_;
};

[[nodiscard]] inline ThreadPoolExecutor make_executor(thread_pool& pool) noexcept {
    return ThreadPoolExecutor(pool);
}

[[nodiscard]] inline EventLoopExecutor make_executor(EventLoop& loop) noexcept {
    return EventLoopExecutor(loop);
}

}  // namespace utils

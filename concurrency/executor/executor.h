#pragma once

/**
 * executor 核心：concept + inline_executor + any_executor（C++20）
 *
 * 适配 thread_pool / event_loop 见 concurrency/executor/adapters.h
 */

#include "concurrency/detail/any_invocable.h"

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace utils {

template <class E>
concept executor = requires(E& e, any_invocable<void()>&& f) {
    { e.post(std::move(f)) };
};

template <class E>
concept try_executor = executor<E> && requires(E& e, any_invocable<void()>&& f) {
    { e.try_post(std::move(f)) } -> std::convertible_to<bool>;
};

/** 当前线程同步执行（测试 / 默认） */
class inline_executor {
public:
    template <class F>
    void post(F&& f) {
        std::forward<F>(f)();
    }

    template <class F>
    bool try_post(F&& f) {
        std::forward<F>(f)();
        return true;
    }
};

/** 类型擦除 executor：运行时可换实现 */
class any_executor {
public:
    any_executor() = default;

    template <class E>
        requires executor<std::decay_t<E>> &&
                 (!std::is_same_v<std::decay_t<E>, any_executor>)
    any_executor(E& exec)
        : self_(std::make_shared<model<std::decay_t<E>>>(exec)) {}

    template <class E>
        requires executor<std::decay_t<E>> &&
                 (!std::is_same_v<std::decay_t<E>, any_executor>)
    any_executor(E* exec) : any_executor(*exec) {}

    any_executor(const any_executor&) = default;
    any_executor(any_executor&&) noexcept = default;
    any_executor& operator=(const any_executor&) = default;
    any_executor& operator=(any_executor&&) noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(self_);
    }

    template <class F>
    void post(F&& f) {
        if (!self_) {
            return;
        }
        self_->post(any_invocable<void()>(std::forward<F>(f)));
    }

    template <class F>
    bool try_post(F&& f) {
        if (!self_) {
            return false;
        }
        return self_->try_post(any_invocable<void()>(std::forward<F>(f)));
    }

private:
    struct concept_base {
        virtual ~concept_base() = default;
        virtual void post(any_invocable<void()> f) = 0;
        virtual bool try_post(any_invocable<void()> f) = 0;
    };

    template <class E>
    struct model final : concept_base {
        explicit model(E& e) : exec(&e) {}
        E* exec;

        void post(any_invocable<void()> f) override { exec->post(std::move(f)); }

        bool try_post(any_invocable<void()> f) override {
            if constexpr (requires(E& e, any_invocable<void()>&& x) {
                              {
                                  e.try_post(std::move(x))
                              } -> std::convertible_to<bool>;
                          }) {
                return exec->try_post(std::move(f));
            } else {
                exec->post(std::move(f));
                return true;
            }
        }
    };

    std::shared_ptr<concept_base> self_;
};

[[nodiscard]] inline inline_executor make_inline_executor() noexcept {
    return inline_executor{};
}

}  // namespace utils

#pragma once

/**
 * Executor 核心：concept + InlineExecutor + AnyExecutor（C++20）
 *
 * 适配 thread_pool / EventLoop 见 executor/adapters.h
 */

#include "functional/any_invocable.h"

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace utils {

template <class E>
concept Executor = requires(E& e, any_invocable<void()>&& f) {
    { e.post(std::move(f)) };
};

template <class E>
concept TryExecutor = Executor<E> && requires(E& e, any_invocable<void()>&& f) {
    { e.try_post(std::move(f)) } -> std::convertible_to<bool>;
};

/** 当前线程同步执行（测试 / 默认） */
class InlineExecutor {
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

/** 类型擦除 Executor：运行时可换实现 */
class AnyExecutor {
public:
    AnyExecutor() = default;

    template <class E>
        requires Executor<std::decay_t<E>> &&
                 (!std::is_same_v<std::decay_t<E>, AnyExecutor>)
    AnyExecutor(E& exec)
        : self_(std::make_shared<Model<std::decay_t<E>>>(exec)) {}

    template <class E>
        requires Executor<std::decay_t<E>> &&
                 (!std::is_same_v<std::decay_t<E>, AnyExecutor>)
    AnyExecutor(E* exec) : AnyExecutor(*exec) {}

    AnyExecutor(const AnyExecutor&) = default;
    AnyExecutor(AnyExecutor&&) noexcept = default;
    AnyExecutor& operator=(const AnyExecutor&) = default;
    AnyExecutor& operator=(AnyExecutor&&) noexcept = default;

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
    struct Concept {
        virtual ~Concept() = default;
        virtual void post(any_invocable<void()> f) = 0;
        virtual bool try_post(any_invocable<void()> f) = 0;
    };

    template <class E>
    struct Model final : Concept {
        explicit Model(E& e) : exec(&e) {}
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

    std::shared_ptr<Concept> self_;
};

[[nodiscard]] inline InlineExecutor make_inline_executor() noexcept {
    return InlineExecutor{};
}

}  // namespace utils

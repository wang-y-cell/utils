#pragma once

// executor 内部使用的 move-only 类型擦除回调。

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace utils {

template <class Signature>
class any_invocable;

template <class R, class... Args>
class any_invocable<R(Args...)> {
public:
    any_invocable() noexcept = default;
    any_invocable(std::nullptr_t) noexcept {}

    any_invocable(const any_invocable&) = delete;
    any_invocable& operator=(const any_invocable&) = delete;
    any_invocable(any_invocable&&) noexcept = default;
    any_invocable& operator=(any_invocable&&) noexcept = default;

    any_invocable& operator=(std::nullptr_t) noexcept {
        target_.reset();
        return *this;
    }

    template <class F>
        requires(!std::is_same_v<std::decay_t<F>, any_invocable> &&
                 std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
    any_invocable(F&& f)
        : target_(
              std::make_unique<model<std::decay_t<F>>>(std::forward<F>(f))) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(target_);
    }

    void reset() noexcept { target_.reset(); }

    R operator()(Args... args) {
        if (!target_) {
            throw std::bad_function_call();
        }
        return target_->invoke(std::forward<Args>(args)...);
    }

    void swap(any_invocable& other) noexcept {
        target_.swap(other.target_);
    }

private:
    struct concept_base {
        virtual ~concept_base() = default;
        virtual R invoke(Args&&... args) = 0;
    };

    template <class F>
    struct model final : concept_base {
        template <class Fn>
        explicit model(Fn&& fn) : callable(std::forward<Fn>(fn)) {}

        R invoke(Args&&... args) override {
            return std::invoke(callable, std::forward<Args>(args)...);
        }

        F callable;
    };

    std::unique_ptr<concept_base> target_;
};

}  // namespace utils

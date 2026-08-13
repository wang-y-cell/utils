#pragma once

/**
 * scope_guard / scope_success / scope_fail / UTILS_DEFER — RAII 收尾（C++20）
 *
 * 日常用法：
 *   auto g = utils::make_scope_guard([&] { close(fd); });
 *   UTILS_DEFER { unlock(); };
 *   utils::scope_fail rollback{[&] { flag = old; }};
 *
 * 异常策略：清理函数应不抛；栈展开中再抛 → std::terminate。
 */

#include <exception>
#include <type_traits>
#include <utility>

namespace utils {

template <class F>
class scope_guard {
public:
    static_assert(std::is_invocable_v<F&>, "scope_guard F must be invocable");

    explicit scope_guard(F&& f) noexcept(
        std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(f)), active_(true) {}

    explicit scope_guard(const F& f) noexcept(
        std::is_nothrow_copy_constructible_v<F>)
        : func_(f), active_(true) {}

    scope_guard(scope_guard&& other) noexcept(
        std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(other.func_)), active_(other.active_) {
        other.active_ = false;
    }

    scope_guard(const scope_guard&) = delete;
    scope_guard& operator=(const scope_guard&) = delete;
    scope_guard& operator=(scope_guard&&) = delete;

    ~scope_guard() noexcept {
        if (active_) {
            func_();
        }
    }

    /** @brief 销毁这个对象时，不执行这个函数 */
    void dismiss() noexcept { active_ = false; }
    /** @brief 销毁这个对象时，执行这个函数 */
    void release() noexcept { dismiss(); }

    /** @brief 是否激活 */
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    F func_; //当这个对象被销毁时，会执行这个函数
    bool active_; //是否激活
};

template <class F>
[[nodiscard]] scope_guard<std::decay_t<F>> make_scope_guard(F&& f) {
    return scope_guard<std::decay_t<F>>(std::forward<F>(f));
}

template <class F>
class scope_success {
public:
    explicit scope_success(F&& f) noexcept(
        std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(f)),
          active_(true),
          exception_count_(std::uncaught_exceptions()) {} //当前的异常还没有被catch到的数量

    explicit scope_success(const F& f) noexcept(
        std::is_nothrow_copy_constructible_v<F>)
        : func_(f),
          active_(true),
          exception_count_(std::uncaught_exceptions()) {}

    scope_success(scope_success&& other) noexcept(
        std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(other.func_)),
          active_(other.active_),
          exception_count_(other.exception_count_) {
        other.active_ = false;
    }

    scope_success(const scope_success&) = delete;
    scope_success& operator=(const scope_success&) = delete;
    scope_success& operator=(scope_success&&) = delete;

    ~scope_success() noexcept {
        /** 如果当前的异常还没有被catch到的数量等于初始化时的异常还没有被catch到的数量，则执行这个函数 */
        if (active_ && std::uncaught_exceptions() == exception_count_) {
            func_();
        }
    }

    void dismiss() noexcept { active_ = false; }
    void release() noexcept { dismiss(); }

private:
    F func_;
    bool active_;
    int exception_count_; //当前的异常还没有被catch到的数量
};

template <class F>
[[nodiscard]] scope_success<std::decay_t<F>> make_scope_success(F&& f) {
    return scope_success<std::decay_t<F>>(std::forward<F>(f));
}

template <class F>
class scope_fail {
public:
    explicit scope_fail(F&& f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(f)),
          active_(true),
          exception_count_(std::uncaught_exceptions()) {}

    explicit scope_fail(const F& f) noexcept(
        std::is_nothrow_copy_constructible_v<F>)
        : func_(f),
          active_(true),
          exception_count_(std::uncaught_exceptions()) {}

    scope_fail(scope_fail&& other) noexcept(
        std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(other.func_)),
          active_(other.active_),
          exception_count_(other.exception_count_) {
        other.active_ = false;
    }

    scope_fail(const scope_fail&) = delete;
    scope_fail& operator=(const scope_fail&) = delete;
    scope_fail& operator=(scope_fail&&) = delete;

    ~scope_fail() noexcept {
        if (active_ && std::uncaught_exceptions() > exception_count_) {
            func_();
        }
    }

    void dismiss() noexcept { active_ = false; }
    void release() noexcept { dismiss(); }

private:
    F func_;
    bool active_;
    int exception_count_;
};

template <class F>
[[nodiscard]] scope_fail<std::decay_t<F>> make_scope_fail(F&& f) {
    return scope_fail<std::decay_t<F>>(std::forward<F>(f));
}

namespace detail {

struct defer_factory {
    template <class F>
    [[nodiscard]] scope_guard<std::decay_t<F>> operator<<(F&& f) const {
        return scope_guard<std::decay_t<F>>(std::forward<F>(f));
    }
};

}  // namespace detail

}  // namespace utils

#define UTILS_CONCAT_INNER(a, b) a##b
#define UTILS_CONCAT(a, b) UTILS_CONCAT_INNER(a, b)

/**
 * UTILS_DEFER { cleanup(); };
 * 在作用域结束时执行；可用 auto& 变量名若需 dismiss，请改用 make_scope_guard。
 */
#define UTILS_DEFER                                   \
    const auto UTILS_CONCAT(_utils_defer_, __LINE__) = \
        ::utils::detail::defer_factory{} << [&]()

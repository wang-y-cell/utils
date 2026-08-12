#pragma once

/**
 * function_ref<R(Args...)> — 非拥有、可拷贝的类型擦除回调观察（C++20）
 *
 * 不延长被绑定对象寿命；调用方保证回调存活期间目标有效。
 * 空引用调用：assert（NDEBUG 下为未定义行为）。
 */

#include <cassert>
#include <functional>
#include <type_traits>
#include <utility>

namespace utils {

template <class Sig>
class function_ref;

namespace detail {

template <class T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <class T>
constexpr bool is_function_ref_v = false;
template <class Sig>
constexpr bool is_function_ref_v<function_ref<Sig>> = true;

}  // namespace detail

template <class R, class... Args>
class function_ref<R(Args...)> {
public:
    using result_type = R;

    function_ref() noexcept = default;
    function_ref(std::nullptr_t) noexcept {}

    function_ref(const function_ref&) noexcept = default;
    function_ref& operator=(const function_ref&) noexcept = default;

    template <class F,
              std::enable_if_t<
                  !detail::is_function_ref_v<detail::remove_cvref_t<F>> &&
                      !std::is_same_v<detail::remove_cvref_t<F>, std::nullptr_t> &&
                      std::is_invocable_r_v<R, F&, Args...>,
                  int> = 0>
    function_ref(F&& f) noexcept
        : obj_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          thunk_(+[](void* obj, Args... args) -> R {
              using FDecayed = detail::remove_cvref_t<F>;
              // 绑定的是原对象引用，不是 decay 后的副本
              auto& ref = *static_cast<std::add_pointer_t<F>>(obj);
              return static_cast<R>(
                  std::invoke(ref, std::forward<Args>(args)...));
              (void)sizeof(FDecayed);
          }) {}

    // 函数指针
    function_ref(R (*fp)(Args...)) noexcept
        : obj_(reinterpret_cast<void*>(fp)),
          thunk_(fp ? +[](void* obj, Args... args) -> R {
              auto* f = reinterpret_cast<R (*)(Args...)>(obj);
              return f(std::forward<Args>(args)...);
          }
                    : nullptr) {}

    R operator()(Args... args) const {
        assert(thunk_ != nullptr && "function_ref: empty");
        return thunk_(obj_, std::forward<Args>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return thunk_ != nullptr;
    }

    void swap(function_ref& other) noexcept {
        std::swap(obj_, other.obj_);
        std::swap(thunk_, other.thunk_);
    }

private:
    using thunk_t = R (*)(void*, Args...);
    void* obj_ = nullptr;
    thunk_t thunk_ = nullptr;
};

template <class R, class... Args>
class function_ref<R(Args...) noexcept> {
public:
    using result_type = R;

    function_ref() noexcept = default;
    function_ref(std::nullptr_t) noexcept {}

    function_ref(const function_ref&) noexcept = default;
    function_ref& operator=(const function_ref&) noexcept = default;

    template <class F,
              std::enable_if_t<
                  !detail::is_function_ref_v<detail::remove_cvref_t<F>> &&
                      std::is_nothrow_invocable_r_v<R, F&, Args...>,
                  int> = 0>
    function_ref(F&& f) noexcept
        : obj_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          thunk_(+[](void* obj, Args... args) noexcept -> R {
              auto& ref = *static_cast<std::add_pointer_t<F>>(obj);
              return static_cast<R>(
                  std::invoke(ref, std::forward<Args>(args)...));
          }) {}

    function_ref(R (*fp)(Args...) noexcept) noexcept
        : obj_(reinterpret_cast<void*>(fp)),
          thunk_(fp ? +[](void* obj, Args... args) noexcept -> R {
              auto* f = reinterpret_cast<R (*)(Args...) noexcept>(obj);
              return f(std::forward<Args>(args)...);
          }
                    : nullptr) {}

    R operator()(Args... args) const noexcept {
        assert(thunk_ != nullptr && "function_ref: empty");
        return thunk_(obj_, std::forward<Args>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return thunk_ != nullptr;
    }

private:
    using thunk_t = R (*)(void*, Args...) noexcept;
    void* obj_ = nullptr;
    thunk_t thunk_ = nullptr;
};

// const 限定：绑定到 const 可调用
template <class R, class... Args>
class function_ref<R(Args...) const> {
public:
    using result_type = R;

    function_ref() noexcept = default;
    function_ref(std::nullptr_t) noexcept {}

    function_ref(const function_ref&) noexcept = default;
    function_ref& operator=(const function_ref&) noexcept = default;

    template <class F,
              std::enable_if_t<
                  !detail::is_function_ref_v<detail::remove_cvref_t<F>> &&
                      std::is_invocable_r_v<R, const detail::remove_cvref_t<F>&,
                                            Args...>,
                  int> = 0>
    function_ref(F&& f) noexcept
        : obj_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          thunk_(+[](void* obj, Args... args) -> R {
              const auto& ref =
                  *static_cast<const detail::remove_cvref_t<F>*>(obj);
              return static_cast<R>(
                  std::invoke(ref, std::forward<Args>(args)...));
          }) {}

    R operator()(Args... args) const {
        assert(thunk_ != nullptr && "function_ref: empty");
        return thunk_(obj_, std::forward<Args>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return thunk_ != nullptr;
    }

private:
    using thunk_t = R (*)(void*, Args...);
    void* obj_ = nullptr;
    thunk_t thunk_ = nullptr;
};

}  // namespace utils

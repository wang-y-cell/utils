#pragma once

/**
 * any_invocable<R(Args...)> — 可移动、可为空、move-only 类型擦除回调（C++20）
 *
 * 相对 std::function：不要求 CopyConstructible；带 32 字节 SBO。
 * 空调用抛 std::bad_function_call。
 */

#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace utils {

template <class Sig>
class any_invocable;

namespace detail {

inline constexpr std::size_t any_invocable_sbo_size = 32;
inline constexpr std::size_t any_invocable_sbo_align = alignof(std::max_align_t);

template <class T>
struct is_any_invocable : std::false_type {};
template <class Sig>
struct is_any_invocable<any_invocable<Sig>> : std::true_type {};

template <class R, class... Args>
struct any_invocable_vtable {
    R (*invoke)(void* storage, Args&&... args);
    void (*destroy)(void* storage) noexcept;
    void (*move_to)(void* dst, void* src) noexcept;
    bool local;
};

template <class F, class R, class... Args>
struct any_invocable_ops {
    static constexpr bool use_sbo =
        sizeof(F) <= any_invocable_sbo_size &&
        alignof(F) <= any_invocable_sbo_align &&
        std::is_nothrow_move_constructible_v<F>;

    static R invoke_local(void* storage, Args&&... args) {
        return static_cast<R>(
            std::invoke(*std::launder(reinterpret_cast<F*>(storage)),
                        std::forward<Args>(args)...));
    }

    static R invoke_heap(void* storage, Args&&... args) {
        auto* ptr = *std::launder(reinterpret_cast<F**>(storage));
        return static_cast<R>(
            std::invoke(*ptr, std::forward<Args>(args)...));
    }

    static void destroy_local(void* storage) noexcept {
        std::destroy_at(std::launder(reinterpret_cast<F*>(storage)));
    }

    static void destroy_heap(void* storage) noexcept {
        auto* ptr = *std::launder(reinterpret_cast<F**>(storage));
        delete ptr;
    }

    static void move_local(void* dst, void* src) noexcept {
        std::construct_at(reinterpret_cast<F*>(dst),
                          std::move(*std::launder(reinterpret_cast<F*>(src))));
        std::destroy_at(std::launder(reinterpret_cast<F*>(src)));
    }

    static void move_heap(void* dst, void* src) noexcept {
        auto* ptr = *std::launder(reinterpret_cast<F**>(src));
        *reinterpret_cast<F**>(dst) = ptr;
        *reinterpret_cast<F**>(src) = nullptr;
    }

    static const any_invocable_vtable<R, Args...>* table() {
        static const any_invocable_vtable<R, Args...> v = use_sbo
            ? any_invocable_vtable<R, Args...>{
                  &invoke_local, &destroy_local, &move_local, true}
            : any_invocable_vtable<R, Args...>{
                  &invoke_heap, &destroy_heap, &move_heap, false};
        return &v;
    }
};

}  // namespace detail

template <class R, class... Args>
class any_invocable<R(Args...)> {
public:
    using result_type = R;

    any_invocable() noexcept = default;
    any_invocable(std::nullptr_t) noexcept {}

    any_invocable(const any_invocable&) = delete;
    any_invocable& operator=(const any_invocable&) = delete;

    any_invocable(any_invocable&& other) noexcept { move_from(std::move(other)); }

    any_invocable& operator=(any_invocable&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    any_invocable& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    template <
        class F,
        class FD = std::decay_t<F>,
        std::enable_if_t<!detail::is_any_invocable<FD>::value &&
                             !std::is_same_v<FD, std::nullptr_t> &&
                             std::is_invocable_r_v<R, FD&, Args...>,
                         int> = 0>
    any_invocable(F&& f) {
        emplace(std::forward<F>(f));
    }

    // 从 std::function 迁入（非空时）
    any_invocable(std::function<R(Args...)> f) {
        if (f) {
            emplace(std::move(f));
        }
    }

    ~any_invocable() { reset(); }

    void reset() noexcept {
        if (vtable_) {
            vtable_->destroy(storage());
            vtable_ = nullptr;
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return vtable_ != nullptr;
    }

    R operator()(Args... args) {
        if (!vtable_) {
            throw std::bad_function_call();
        }
        return vtable_->invoke(storage(), std::forward<Args>(args)...);
    }

    void swap(any_invocable& other) noexcept {
        any_invocable tmp(std::move(*this));
        *this = std::move(other);
        other = std::move(tmp);
    }

private:
    void move_from(any_invocable&& other) noexcept {
        vtable_ = other.vtable_;
        if (vtable_) {
            vtable_->move_to(storage(), other.storage());
            other.vtable_ = nullptr;
        }
    }

    template <class F>
    void emplace(F&& f) {
        using FD = std::decay_t<F>;
        using ops = detail::any_invocable_ops<FD, R, Args...>;
        vtable_ = ops::table();
        if constexpr (ops::use_sbo) {
            std::construct_at(reinterpret_cast<FD*>(storage()),
                              std::forward<F>(f));
        } else {
            *reinterpret_cast<FD**>(storage()) =
                new FD(std::forward<F>(f));
        }
    }

    void* storage() noexcept { return static_cast<void*>(buf_); }
    const void* storage() const noexcept {
        return static_cast<const void*>(buf_);
    }

    alignas(detail::any_invocable_sbo_align) unsigned char
        buf_[detail::any_invocable_sbo_size]{};
    const detail::any_invocable_vtable<R, Args...>* vtable_ = nullptr;
};

template <class R, class... Args>
class any_invocable<R(Args...) const> {
public:
    using result_type = R;

    any_invocable() noexcept = default;
    any_invocable(std::nullptr_t) noexcept {}

    any_invocable(const any_invocable&) = delete;
    any_invocable& operator=(const any_invocable&) = delete;

    any_invocable(any_invocable&& other) noexcept { move_from(std::move(other)); }

    any_invocable& operator=(any_invocable&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    any_invocable& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    template <
        class F,
        class FD = std::decay_t<F>,
        std::enable_if_t<!detail::is_any_invocable<FD>::value &&
                             std::is_invocable_r_v<R, const FD&, Args...>,
                         int> = 0>
    any_invocable(F&& f) {
        emplace(std::forward<F>(f));
    }

    ~any_invocable() { reset(); }

    void reset() noexcept {
        if (vtable_) {
            vtable_->destroy(storage());
            vtable_ = nullptr;
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return vtable_ != nullptr;
    }

    R operator()(Args... args) const {
        if (!vtable_) {
            throw std::bad_function_call();
        }
        return vtable_->invoke(storage(), std::forward<Args>(args)...);
    }

private:
    struct vtable_type {
        R (*invoke)(const void* storage, Args&&... args);
        void (*destroy)(void* storage) noexcept;
        void (*move_to)(void* dst, void* src) noexcept;
    };

    template <class F>
    struct ops {
        static constexpr bool use_sbo =
            sizeof(F) <= detail::any_invocable_sbo_size &&
            alignof(F) <= detail::any_invocable_sbo_align &&
            std::is_nothrow_move_constructible_v<F>;

        static R invoke_local(const void* storage, Args&&... args) {
            return static_cast<R>(std::invoke(
                *std::launder(reinterpret_cast<const F*>(storage)),
                std::forward<Args>(args)...));
        }
        static R invoke_heap(const void* storage, Args&&... args) {
            const F* ptr =
                *std::launder(reinterpret_cast<F* const*>(storage));
            return static_cast<R>(
                std::invoke(*ptr, std::forward<Args>(args)...));
        }
        static void destroy_local(void* storage) noexcept {
            std::destroy_at(std::launder(reinterpret_cast<F*>(storage)));
        }
        static void destroy_heap(void* storage) noexcept {
            delete *std::launder(reinterpret_cast<F**>(storage));
        }
        static void move_local(void* dst, void* src) noexcept {
            std::construct_at(
                reinterpret_cast<F*>(dst),
                std::move(*std::launder(reinterpret_cast<F*>(src))));
            std::destroy_at(std::launder(reinterpret_cast<F*>(src)));
        }
        static void move_heap(void* dst, void* src) noexcept {
            *reinterpret_cast<F**>(dst) =
                *std::launder(reinterpret_cast<F**>(src));
            *reinterpret_cast<F**>(src) = nullptr;
        }
        static const vtable_type* table() {
            static const vtable_type v =
                use_sbo ? vtable_type{&invoke_local, &destroy_local, &move_local}
                        : vtable_type{&invoke_heap, &destroy_heap, &move_heap};
            return &v;
        }
    };

    void move_from(any_invocable&& other) noexcept {
        vtable_ = other.vtable_;
        if (vtable_) {
            vtable_->move_to(storage(), other.storage());
            other.vtable_ = nullptr;
        }
    }

    template <class F>
    void emplace(F&& f) {
        using FD = std::decay_t<F>;
        using O = ops<FD>;
        vtable_ = O::table();
        if constexpr (O::use_sbo) {
            std::construct_at(reinterpret_cast<FD*>(storage()),
                              std::forward<F>(f));
        } else {
            *reinterpret_cast<FD**>(storage()) =
                new FD(std::forward<F>(f));
        }
    }

    void* storage() noexcept { return static_cast<void*>(buf_); }
    const void* storage() const noexcept {
        return static_cast<const void*>(buf_);
    }

    alignas(detail::any_invocable_sbo_align) unsigned char
        buf_[detail::any_invocable_sbo_size]{};
    const vtable_type* vtable_ = nullptr;
};

template <class R, class... Args>
class any_invocable<R(Args...) noexcept> {
public:
    using result_type = R;

    any_invocable() noexcept = default;
    any_invocable(std::nullptr_t) noexcept {}

    any_invocable(const any_invocable&) = delete;
    any_invocable& operator=(const any_invocable&) = delete;

    any_invocable(any_invocable&& other) noexcept { move_from(std::move(other)); }

    any_invocable& operator=(any_invocable&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    template <
        class F,
        class FD = std::decay_t<F>,
        std::enable_if_t<!detail::is_any_invocable<FD>::value &&
                             std::is_nothrow_invocable_r_v<R, FD&, Args...>,
                         int> = 0>
    any_invocable(F&& f) {
        emplace(std::forward<F>(f));
    }

    ~any_invocable() { reset(); }

    void reset() noexcept {
        if (vtable_) {
            vtable_->destroy(storage());
            vtable_ = nullptr;
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return vtable_ != nullptr;
    }

    R operator()(Args... args) noexcept {
        assert(vtable_);
        return vtable_->invoke(storage(), std::forward<Args>(args)...);
    }

private:
    struct vtable_type {
        R (*invoke)(void* storage, Args&&... args) noexcept;
        void (*destroy)(void* storage) noexcept;
        void (*move_to)(void* dst, void* src) noexcept;
    };

    template <class F>
    struct ops {
        static constexpr bool use_sbo =
            sizeof(F) <= detail::any_invocable_sbo_size &&
            alignof(F) <= detail::any_invocable_sbo_align &&
            std::is_nothrow_move_constructible_v<F>;

        static R invoke_local(void* storage, Args&&... args) noexcept {
            return static_cast<R>(std::invoke(
                *std::launder(reinterpret_cast<F*>(storage)),
                std::forward<Args>(args)...));
        }
        static R invoke_heap(void* storage, Args&&... args) noexcept {
            return static_cast<R>(std::invoke(
                **std::launder(reinterpret_cast<F**>(storage)),
                std::forward<Args>(args)...));
        }
        static void destroy_local(void* storage) noexcept {
            std::destroy_at(std::launder(reinterpret_cast<F*>(storage)));
        }
        static void destroy_heap(void* storage) noexcept {
            delete *std::launder(reinterpret_cast<F**>(storage));
        }
        static void move_local(void* dst, void* src) noexcept {
            std::construct_at(
                reinterpret_cast<F*>(dst),
                std::move(*std::launder(reinterpret_cast<F*>(src))));
            std::destroy_at(std::launder(reinterpret_cast<F*>(src)));
        }
        static void move_heap(void* dst, void* src) noexcept {
            *reinterpret_cast<F**>(dst) =
                *std::launder(reinterpret_cast<F**>(src));
            *reinterpret_cast<F**>(src) = nullptr;
        }
        static const vtable_type* table() {
            static const vtable_type v =
                use_sbo ? vtable_type{&invoke_local, &destroy_local, &move_local}
                        : vtable_type{&invoke_heap, &destroy_heap, &move_heap};
            return &v;
        }
    };

    void move_from(any_invocable&& other) noexcept {
        vtable_ = other.vtable_;
        if (vtable_) {
            vtable_->move_to(storage(), other.storage());
            other.vtable_ = nullptr;
        }
    }

    template <class F>
    void emplace(F&& f) {
        using FD = std::decay_t<F>;
        using O = ops<FD>;
        vtable_ = O::table();
        if constexpr (O::use_sbo) {
            std::construct_at(reinterpret_cast<FD*>(storage()),
                              std::forward<F>(f));
        } else {
            *reinterpret_cast<FD**>(storage()) =
                new FD(std::forward<F>(f));
        }
    }

    void* storage() noexcept { return static_cast<void*>(buf_); }

    alignas(detail::any_invocable_sbo_align) unsigned char
        buf_[detail::any_invocable_sbo_size]{};
    const vtable_type* vtable_ = nullptr;
};

}  // namespace utils

#pragma once

/**
 * Expected<T, E> / Result<T> — 错误传递，少用异常当控制流（C++20 header-only）
 *
 * 详细可运行教程：result/expected_demo.cpp
 *   cmake --build build --target expected_demo
 *
 * 速览：
 *   Result<int> r = result_ok(42);
 *   if (!r) { use(r.error()); } else { use(*r); }
 *   auto x = parse().and_then([](int n) -> Result<int> { return result_ok(n * 2); });
 *
 * 业务失败请 return result_err / unexpected，不要用异常当控制流。
 * value() 在无值时抛 BadExpectedAccess，仅作未检查访问的兜底。
 */

#include <cassert>
#include <exception>
#include <functional>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

namespace utils {

struct unexpect_t {
    explicit unexpect_t() = default;
};
inline constexpr unexpect_t unexpect{};

class BadExpectedAccess : public std::exception {
public:
    const char* what() const noexcept override {
        return "BadExpectedAccess: Expected has no value";
    }
};

/**
 * Unexpected<E> — 错误传递，少用异常当控制流（C++20 header-only）
 */
template <class E>
class Unexpected {
public:
    static_assert(!std::is_void_v<E>, "Unexpected<void> is ill-formed");

    constexpr Unexpected(const Unexpected&) = default;
    constexpr Unexpected(Unexpected&&) = default;
    constexpr Unexpected& operator=(const Unexpected&) = default;
    constexpr Unexpected& operator=(Unexpected&&) = default;

    //
    template <class Err = E,
              std::enable_if_t<
                  !std::is_same_v<std::remove_cvref_t<Err>, Unexpected> &&
                      std::is_constructible_v<E, Err>,
                  int> = 0>
    constexpr explicit Unexpected(Err&& e) noexcept(
        std::is_nothrow_constructible_v<E, Err>)
        : error_(std::forward<Err>(e)) {}

    constexpr E& error() & noexcept { return error_; }
    constexpr const E& error() const& noexcept { return error_; }
    constexpr E&& error() && noexcept { return std::move(error_); }
    constexpr const E&& error() const&& noexcept { return std::move(error_); }

private:
    E error_;
};

template <class E>
Unexpected(E) -> Unexpected<E>; //当没有定义类型的时候，会自动推导类型

/// 工厂函数
template <class E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> unexpected(E&& e) {
    return Unexpected<std::decay_t<E>>(std::forward<E>(e));
}

namespace detail {

template <class U>
struct is_unexpected : std::false_type {};
template <class G>
struct is_unexpected<Unexpected<G>> : std::true_type {};
template <class U>
inline constexpr bool is_unexpected_v = is_unexpected<U>::value;

/**
 * ExpectedStorage — 存储T或E的实际数据（C++20 header-only）
 * 这个类是 Expected 内部的底层存储层：
 * 在同一块内存里二选一地存成功值 T 或错误值 E，并管好构造/析构/赋值。
 */
template <class T, class E>
class ExpectedStorage {
public:
    ExpectedStorage() = delete; //显示删除默认构造函数,禁止无参构造,这里写出来是为了表明意图

    template <class... Args>
    explicit ExpectedStorage(std::in_place_t, Args&&... args) //第一个参数表示存储的是T对象
        : has_(true) {
        //在as_value()中的地址上重新构造T对象
        std::construct_at(std::addressof(as_value()), std::forward<Args>(args)...);
    }

    template <class... Args>
    explicit ExpectedStorage(unexpect_t, Args&&... args) : has_(false) { //第一个参数表示存储的是E对象
        std::construct_at(std::addressof(as_error()), std::forward<Args>(args)...);
    }

    ExpectedStorage(const ExpectedStorage& other) : has_(other.has_) {
        if (other.has_) { //如果other存储的是T对象,则在as_value()中的地址上重新构造T对象
            std::construct_at(std::addressof(as_value()), other.as_value());
        } else { //如果other存储的是E对象,则在as_error()中的地址上重新构造E对象
            std::construct_at(std::addressof(as_error()), other.as_error());
        }
    }

    ExpectedStorage(ExpectedStorage&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> &&
        std::is_nothrow_move_constructible_v<E>)
        : has_(other.has_) {
        if (other.has_) {
            std::construct_at(std::addressof(as_value()),
                              std::move(other.as_value()));
        } else {
            std::construct_at(std::addressof(as_error()),
                              std::move(other.as_error()));
        }
    }

    ~ExpectedStorage() { destroy(); }

    ExpectedStorage& operator=(const ExpectedStorage&) = delete;
    ExpectedStorage& operator=(ExpectedStorage&&) = delete;

    [[nodiscard]] bool has_value() const noexcept { return has_; }

    //as_value()用来返回T对象的buf地址的引用
    T& as_value() & noexcept {
        return *std::launder(reinterpret_cast<T*>(buf_)); ///用 std::launder 告知编译器 buf 地址现在有新的 T 对象
    }
    const T& as_value() const& noexcept {
        return *std::launder(reinterpret_cast<const T*>(buf_)); ///用 std::launder 告知编译器 buf 地址现在有新的 T 对象
    }
    T&& as_value() && noexcept {
        return std::move(*std::launder(reinterpret_cast<T*>(buf_))); ///用 std::launder 告知编译器 buf 地址现在有新的 T 对象
    }

    //as_error()用来返回E对象的buf地址的引用
    E& as_error() & noexcept {
        return *std::launder(reinterpret_cast<E*>(buf_)); ///用 std::launder 告知编译器 buf 地址现在有新的 E 对象
    }
    const E& as_error() const& noexcept {
        return *std::launder(reinterpret_cast<const E*>(buf_)); ///用 std::launder 告知编译器 buf 地址现在有新的 E 对象
    }
    E&& as_error() && noexcept {
        return std::move(*std::launder(reinterpret_cast<E*>(buf_))); ///用 std::launder 告知编译器 buf 地址现在有新的 E 对象
    }

    void destroy() noexcept {
        if (has_) {
            std::destroy_at(std::addressof(as_value()));
        } else {
            std::destroy_at(std::addressof(as_error()));
        }
    }

    void construct_value(const T& v) {
        std::construct_at(std::addressof(as_value()), v);
        has_ = true;
    }
    void construct_value(T&& v) {
        std::construct_at(std::addressof(as_value()), std::move(v));
        has_ = true;
    }
    void construct_error(const E& e) {
        std::construct_at(std::addressof(as_error()), e);
        has_ = false;
    }
    void construct_error(E&& e) {
        std::construct_at(std::addressof(as_error()), std::move(e));
        has_ = false;
    }

    void assign_from(const ExpectedStorage& other) {
        if (has_ && other.has_) {
            as_value() = other.as_value();
        } else if (!has_ && !other.has_) {
            as_error() = other.as_error();
        } else if (has_ && !other.has_) {
            destroy();
            construct_error(other.as_error());
        } else {
            destroy();
            construct_value(other.as_value());
        }
    }

    void assign_from(ExpectedStorage&& other) {
        if (has_ && other.has_) {
            as_value() = std::move(other.as_value());
        } else if (!has_ && !other.has_) {
            as_error() = std::move(other.as_error());
        } else if (has_ && !other.has_) {
            destroy();
            construct_error(std::move(other.as_error()));
        } else {
            destroy();
            construct_value(std::move(other.as_value()));
        }
    }

private:
    static constexpr std::size_t kSize = ///获取T和E中较大的那个类型的字节数
        sizeof(T) > sizeof(E) ? sizeof(T) : sizeof(E);
    static constexpr std::size_t kAlign = ///获取T和E中较大的那个类型的对齐方式
        alignof(T) > alignof(E) ? alignof(T) : alignof(E);

    alignas(kAlign) unsigned char buf_[kSize]{}; ///对齐到kAlign,大小为kSize的数组
    ///has_用于判断当前存储的是T还是E,true表示存储的是T,false表示存储的是E
    bool has_;
};

template <class E>
class ExpectedVoidStorage {
public:
    ExpectedVoidStorage() noexcept : has_(true) {}

    template <class... Args>
    explicit ExpectedVoidStorage(unexpect_t, Args&&... args) : has_(false) {
        std::construct_at(std::addressof(as_error()), std::forward<Args>(args)...);
    }

    ExpectedVoidStorage(const ExpectedVoidStorage& other) : has_(other.has_) {
        if (!other.has_) {
            std::construct_at(std::addressof(as_error()), other.as_error());
        }
    }

    ExpectedVoidStorage(ExpectedVoidStorage&& other) noexcept(
        std::is_nothrow_move_constructible_v<E>)
        : has_(other.has_) {
        if (!other.has_) {
            std::construct_at(std::addressof(as_error()),
                              std::move(other.as_error()));
        }
    }

    ~ExpectedVoidStorage() {
        if (!has_) {
            std::destroy_at(std::addressof(as_error()));
        }
    }

    ExpectedVoidStorage& operator=(const ExpectedVoidStorage&) = delete;
    ExpectedVoidStorage& operator=(ExpectedVoidStorage&&) = delete;

    [[nodiscard]] bool has_value() const noexcept { return has_; }

    E& as_error() & noexcept {
        return *std::launder(reinterpret_cast<E*>(buf_));
    }
    const E& as_error() const& noexcept {
        return *std::launder(reinterpret_cast<const E*>(buf_));
    }
    E&& as_error() && noexcept {
        return std::move(*std::launder(reinterpret_cast<E*>(buf_)));
    }

    void assign_from(const ExpectedVoidStorage& other) {
        if (has_ && other.has_) {
            return;
        }
        if (!has_ && !other.has_) {
            as_error() = other.as_error();
            return;
        }
        if (has_ && !other.has_) {
            std::construct_at(std::addressof(as_error()), other.as_error());
            has_ = false;
            return;
        }
        std::destroy_at(std::addressof(as_error()));
        has_ = true;
    }

    void assign_from(ExpectedVoidStorage&& other) {
        if (has_ && other.has_) {
            return;
        }
        if (!has_ && !other.has_) {
            as_error() = std::move(other.as_error());
            return;
        }
        if (has_ && !other.has_) {
            std::construct_at(std::addressof(as_error()),
                              std::move(other.as_error()));
            has_ = false;
            return;
        }
        std::destroy_at(std::addressof(as_error()));
        has_ = true;
    }

private:
    alignas(E) unsigned char buf_[sizeof(E)]{};
    bool has_;
};

}  // namespace detail

template <class T, class E>
class Expected {
    static_assert(!std::is_same_v<std::remove_cv_t<E>, void>);
    static_assert(!std::is_same_v<std::remove_cv_t<T>, unexpect_t>);
    static_assert(!std::is_same_v<std::remove_cv_t<T>, std::in_place_t>);

public:
    using value_type = T;
    using error_type = E;
    using unexpected_type = Unexpected<E>;

    template <class U = T,
              std::enable_if_t<std::is_default_constructible_v<U>, int> = 0>
    Expected() : storage_(std::in_place) {}

    Expected(const Expected&) = default;
    Expected(Expected&&) = default;

    template <class U = T,
              std::enable_if_t<
                  !std::is_same_v<std::remove_cvref_t<U>, Expected> &&
                      !std::is_same_v<std::remove_cvref_t<U>, std::in_place_t> &&
                      !detail::is_unexpected_v<std::remove_cvref_t<U>> &&
                      std::is_constructible_v<T, U>,
                  int> = 0>
    Expected(U&& v) : storage_(std::in_place, std::forward<U>(v)) {}

    template <class G,
              std::enable_if_t<std::is_constructible_v<E, const G&>, int> = 0>
    Expected(const Unexpected<G>& u) : storage_(unexpect, u.error()) {}

    template <class G, std::enable_if_t<std::is_constructible_v<E, G>, int> = 0>
    Expected(Unexpected<G>&& u) : storage_(unexpect, std::move(u.error())) {}

    template <class... Args,
              std::enable_if_t<std::is_constructible_v<T, Args...>, int> = 0>
    explicit Expected(std::in_place_t, Args&&... args)
        : storage_(std::in_place, std::forward<Args>(args)...) {}

    template <class... Args,
              std::enable_if_t<std::is_constructible_v<E, Args...>, int> = 0>
    explicit Expected(unexpect_t, Args&&... args)
        : storage_(unexpect, std::forward<Args>(args)...) {}

    Expected& operator=(const Expected& other) {
        if (this != &other) {
            storage_.assign_from(other.storage_);
        }
        return *this;
    }

    Expected& operator=(Expected&& other) noexcept(
        std::is_nothrow_move_assignable_v<T> &&
        std::is_nothrow_move_constructible_v<T> &&
        std::is_nothrow_move_assignable_v<E> &&
        std::is_nothrow_move_constructible_v<E>) {
        if (this != &other) {
            storage_.assign_from(std::move(other.storage_));
        }
        return *this;
    }

    [[nodiscard]] bool has_value() const noexcept { return storage_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        if (!has_value()) {
            throw BadExpectedAccess{};
        }
        return storage_.as_value();
    }
    const T& value() const& {
        if (!has_value()) {
            throw BadExpectedAccess{};
        }
        return storage_.as_value();
    }
    T&& value() && {
        if (!has_value()) {
            throw BadExpectedAccess{};
        }
        return std::move(storage_.as_value());
    }

    E& error() & {
        assert(!has_value());
        return storage_.as_error();
    }
    const E& error() const& {
        assert(!has_value());
        return storage_.as_error();
    }
    E&& error() && {
        assert(!has_value());
        return std::move(storage_.as_error());
    }

    template <class U>
    T value_or(U&& def) const& {
        return has_value() ? storage_.as_value()
                           : static_cast<T>(std::forward<U>(def));
    }
    template <class U>
    T value_or(U&& def) && {
        return has_value() ? std::move(storage_.as_value())
                           : static_cast<T>(std::forward<U>(def));
    }

    template <class G = E>
    E error_or(G&& def) const& {
        return !has_value() ? storage_.as_error()
                            : static_cast<E>(std::forward<G>(def));
    }

    T& operator*() & noexcept { return storage_.as_value(); }
    const T& operator*() const& noexcept { return storage_.as_value(); }
    T&& operator*() && noexcept { return std::move(storage_.as_value()); }

    T* operator->() noexcept { return std::addressof(storage_.as_value()); }
    const T* operator->() const noexcept {
        return std::addressof(storage_.as_value());
    }

    template <class F>
    auto and_then(F&& f) & {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T&>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f), storage_.as_value());
        }
        return U(unexpect, storage_.as_error());
    }
    template <class F>
    auto and_then(F&& f) const& {
        using U = std::remove_cvref_t<std::invoke_result_t<F, const T&>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f), storage_.as_value());
        }
        return U(unexpect, storage_.as_error());
    }
    template <class F>
    auto and_then(F&& f) && {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T&&>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f), std::move(storage_.as_value()));
        }
        return U(unexpect, std::move(storage_.as_error()));
    }

    template <class F>
    auto transform(F&& f) & {
        using U = std::remove_cv_t<std::invoke_result_t<F, T&>>;
        if constexpr (std::is_void_v<U>) {
            using R = Expected<void, E>;
            if (has_value()) {
                std::invoke(std::forward<F>(f), storage_.as_value());
                return R{};
            }
            return R(unexpect, storage_.as_error());
        } else {
            using R = Expected<U, E>;
            if (has_value()) {
                return R(std::in_place,
                         std::invoke(std::forward<F>(f), storage_.as_value()));
            }
            return R(unexpect, storage_.as_error());
        }
    }
    template <class F>
    auto transform(F&& f) const& {
        using U = std::remove_cv_t<std::invoke_result_t<F, const T&>>;
        if constexpr (std::is_void_v<U>) {
            using R = Expected<void, E>;
            if (has_value()) {
                std::invoke(std::forward<F>(f), storage_.as_value());
                return R{};
            }
            return R(unexpect, storage_.as_error());
        } else {
            using R = Expected<U, E>;
            if (has_value()) {
                return R(std::in_place,
                         std::invoke(std::forward<F>(f), storage_.as_value()));
            }
            return R(unexpect, storage_.as_error());
        }
    }
    template <class F>
    auto transform(F&& f) && {
        using U = std::remove_cv_t<std::invoke_result_t<F, T&&>>;
        if constexpr (std::is_void_v<U>) {
            using R = Expected<void, E>;
            if (has_value()) {
                std::invoke(std::forward<F>(f), std::move(storage_.as_value()));
                return R{};
            }
            return R(unexpect, std::move(storage_.as_error()));
        } else {
            using R = Expected<U, E>;
            if (has_value()) {
                return R(std::in_place,
                         std::invoke(std::forward<F>(f),
                                     std::move(storage_.as_value())));
            }
            return R(unexpect, std::move(storage_.as_error()));
        }
    }

    template <class F>
    auto or_else(F&& f) & {
        using G = std::remove_cvref_t<std::invoke_result_t<F, E&>>;
        if (has_value()) {
            return G(std::in_place, storage_.as_value());
        }
        return std::invoke(std::forward<F>(f), storage_.as_error());
    }
    template <class F>
    auto or_else(F&& f) const& {
        using G = std::remove_cvref_t<std::invoke_result_t<F, const E&>>;
        if (has_value()) {
            return G(std::in_place, storage_.as_value());
        }
        return std::invoke(std::forward<F>(f), storage_.as_error());
    }
    template <class F>
    auto or_else(F&& f) && {
        using G = std::remove_cvref_t<std::invoke_result_t<F, E&&>>;
        if (has_value()) {
            return G(std::in_place, std::move(storage_.as_value()));
        }
        return std::invoke(std::forward<F>(f), std::move(storage_.as_error()));
    }

    template <class F>
    auto transform_error(F&& f) & {
        using G = std::remove_cv_t<std::invoke_result_t<F, E&>>;
        using R = Expected<T, G>;
        if (has_value()) {
            return R(std::in_place, storage_.as_value());
        }
        return R(unexpect, std::invoke(std::forward<F>(f), storage_.as_error()));
    }
    template <class F>
    auto transform_error(F&& f) const& {
        using G = std::remove_cv_t<std::invoke_result_t<F, const E&>>;
        using R = Expected<T, G>;
        if (has_value()) {
            return R(std::in_place, storage_.as_value());
        }
        return R(unexpect, std::invoke(std::forward<F>(f), storage_.as_error()));
    }
    template <class F>
    auto transform_error(F&& f) && {
        using G = std::remove_cv_t<std::invoke_result_t<F, E&&>>;
        using R = Expected<T, G>;
        if (has_value()) {
            return R(std::in_place, std::move(storage_.as_value()));
        }
        return R(unexpect,
                 std::invoke(std::forward<F>(f), std::move(storage_.as_error())));
    }

private:
    detail::ExpectedStorage<T, E> storage_;
};

template <class E>
class Expected<void, E> {
    static_assert(!std::is_same_v<std::remove_cv_t<E>, void>);

public:
    using value_type = void;
    using error_type = E;
    using unexpected_type = Unexpected<E>;

    Expected() noexcept = default;
    Expected(const Expected&) = default;
    Expected(Expected&&) = default;

    template <class G,
              std::enable_if_t<std::is_constructible_v<E, const G&>, int> = 0>
    Expected(const Unexpected<G>& u) : storage_(unexpect, u.error()) {}

    template <class G, std::enable_if_t<std::is_constructible_v<E, G>, int> = 0>
    Expected(Unexpected<G>&& u) : storage_(unexpect, std::move(u.error())) {}

    explicit Expected(std::in_place_t) noexcept : storage_() {}

    template <class... Args,
              std::enable_if_t<std::is_constructible_v<E, Args...>, int> = 0>
    explicit Expected(unexpect_t, Args&&... args)
        : storage_(unexpect, std::forward<Args>(args)...) {}

    Expected& operator=(const Expected& other) {
        if (this != &other) {
            storage_.assign_from(other.storage_);
        }
        return *this;
    }

    Expected& operator=(Expected&& other) noexcept(
        std::is_nothrow_move_assignable_v<E> &&
        std::is_nothrow_move_constructible_v<E>) {
        if (this != &other) {
            storage_.assign_from(std::move(other.storage_));
        }
        return *this;
    }

    [[nodiscard]] bool has_value() const noexcept { return storage_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    void value() const {
        if (!has_value()) {
            throw BadExpectedAccess{};
        }
    }

    E& error() & {
        assert(!has_value());
        return storage_.as_error();
    }
    const E& error() const& {
        assert(!has_value());
        return storage_.as_error();
    }
    E&& error() && {
        assert(!has_value());
        return std::move(storage_.as_error());
    }

    template <class G = E>
    E error_or(G&& def) const& {
        return !has_value() ? storage_.as_error()
                            : static_cast<E>(std::forward<G>(def));
    }

    void operator*() const noexcept {}

    template <class F>
    auto and_then(F&& f) & {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f));
        }
        return U(unexpect, storage_.as_error());
    }
    template <class F>
    auto and_then(F&& f) const& {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f));
        }
        return U(unexpect, storage_.as_error());
    }
    template <class F>
    auto and_then(F&& f) && {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f));
        }
        return U(unexpect, std::move(storage_.as_error()));
    }

    template <class F>
    auto transform(F&& f) & {
        using U = std::remove_cv_t<std::invoke_result_t<F>>;
        if constexpr (std::is_void_v<U>) {
            using R = Expected<void, E>;
            if (has_value()) {
                std::invoke(std::forward<F>(f));
                return R{};
            }
            return R(unexpect, storage_.as_error());
        } else {
            using R = Expected<U, E>;
            if (has_value()) {
                return R(std::in_place, std::invoke(std::forward<F>(f)));
            }
            return R(unexpect, storage_.as_error());
        }
    }
    template <class F>
    auto transform(F&& f) const& {
        using U = std::remove_cv_t<std::invoke_result_t<F>>;
        if constexpr (std::is_void_v<U>) {
            using R = Expected<void, E>;
            if (has_value()) {
                std::invoke(std::forward<F>(f));
                return R{};
            }
            return R(unexpect, storage_.as_error());
        } else {
            using R = Expected<U, E>;
            if (has_value()) {
                return R(std::in_place, std::invoke(std::forward<F>(f)));
            }
            return R(unexpect, storage_.as_error());
        }
    }
    template <class F>
    auto transform(F&& f) && {
        using U = std::remove_cv_t<std::invoke_result_t<F>>;
        if constexpr (std::is_void_v<U>) {
            using R = Expected<void, E>;
            if (has_value()) {
                std::invoke(std::forward<F>(f));
                return R{};
            }
            return R(unexpect, std::move(storage_.as_error()));
        } else {
            using R = Expected<U, E>;
            if (has_value()) {
                return R(std::in_place, std::invoke(std::forward<F>(f)));
            }
            return R(unexpect, std::move(storage_.as_error()));
        }
    }

    template <class F>
    auto or_else(F&& f) & {
        using G = std::remove_cvref_t<std::invoke_result_t<F, E&>>;
        if (has_value()) {
            return G{};
        }
        return std::invoke(std::forward<F>(f), storage_.as_error());
    }
    template <class F>
    auto or_else(F&& f) const& {
        using G = std::remove_cvref_t<std::invoke_result_t<F, const E&>>;
        if (has_value()) {
            return G{};
        }
        return std::invoke(std::forward<F>(f), storage_.as_error());
    }
    template <class F>
    auto or_else(F&& f) && {
        using G = std::remove_cvref_t<std::invoke_result_t<F, E&&>>;
        if (has_value()) {
            return G{};
        }
        return std::invoke(std::forward<F>(f), std::move(storage_.as_error()));
    }

    template <class F>
    auto transform_error(F&& f) & {
        using G = std::remove_cv_t<std::invoke_result_t<F, E&>>;
        using R = Expected<void, G>;
        if (has_value()) {
            return R{};
        }
        return R(unexpect, std::invoke(std::forward<F>(f), storage_.as_error()));
    }
    template <class F>
    auto transform_error(F&& f) const& {
        using G = std::remove_cv_t<std::invoke_result_t<F, const E&>>;
        using R = Expected<void, G>;
        if (has_value()) {
            return R{};
        }
        return R(unexpect, std::invoke(std::forward<F>(f), storage_.as_error()));
    }
    template <class F>
    auto transform_error(F&& f) && {
        using G = std::remove_cv_t<std::invoke_result_t<F, E&&>>;
        using R = Expected<void, G>;
        if (has_value()) {
            return R{};
        }
        return R(unexpect,
                 std::invoke(std::forward<F>(f), std::move(storage_.as_error())));
    }

private:
    detail::ExpectedVoidStorage<E> storage_;
};

template <class T, class E = std::error_code>
[[nodiscard]] Expected<std::decay_t<T>, E> ok(T&& value) {
    return Expected<std::decay_t<T>, E>(std::in_place, std::forward<T>(value));
}

template <class E = std::error_code>
[[nodiscard]] Expected<void, E> ok() {
    return Expected<void, E>(std::in_place);
}

template <class E>
[[nodiscard]] auto err(E&& e) {
    return unexpected(std::forward<E>(e));
}

template <class T>
using Result = Expected<T, std::error_code>;

[[nodiscard]] inline Result<void> result_ok() { return ok<>(); }

template <class T>
[[nodiscard]] inline Result<std::decay_t<T>> result_ok(T&& value) {
    return ok<T, std::error_code>(std::forward<T>(value));
}

[[nodiscard]] inline auto result_err(std::error_code ec) {
    return unexpected(ec);
}

[[nodiscard]] inline auto result_err(std::errc code) {
    return unexpected(std::make_error_code(code));
}

}  // namespace utils

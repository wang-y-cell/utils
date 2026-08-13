#pragma once

/**
 * Expected<T, E> / Result<T> — 错误传递，少用异常当控制流（C++20 header-only）
 *
 * 详细可运行教程：demo/result/expected_demo.cpp
 *   cmake --build build --target demo_expected
 *
 * 速览：
 *   Result<int> r = result_ok(42);
 *   if (!r) { use(r.error()); } else { use(*r); }
 *   auto x = parse().and_then([](int n) -> Result<int> { return result_ok(n * 2); });
 *
 * 业务失败请 return result_err / unexpected，不要用异常当控制流。
 * value() 在无值时抛 BadExpectedAccess，仅作未检查访问的兜底。
 *
 * 结构概览：
 *   Unexpected<E>           — 显式错误包装，避免与 T 构造歧义
 *   detail::ExpectedStorage — Expected<T,E> 底层：二选一存 T 或 E
 *   detail::ExpectedVoidStorage — Expected<void,E> 底层：成功无载荷
 *   Expected<T,E> / Expected<void,E> — 对外 API
 *   ok / err / Result / result_ok / result_err — 工厂与别名
 */

#include <cassert>
#include <exception>
#include <functional>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

namespace utils {

/** @brief 构造标签：表示接下来要构造错误侧（对应 std::unexpect） */
struct unexpect_t {
    explicit unexpect_t() = default;
};
/** @brief unexpect 标签实例，用于 Expected(unexpect, ...) */
inline constexpr unexpect_t unexpect{};

/**
 * @brief 对空 Expected 调用 value() 时抛出
 * @note 属编程错误兜底，不是业务错误通道
 */
class BadExpectedAccess : public std::exception {
public:
    const char* what() const noexcept override {
        return "BadExpectedAccess: Expected has no value";
    }
};

/**
 * @brief 显式错误包装，构造 Expected 失败态时用，避免与 T 的值构造混淆
 * @tparam E 错误类型（不可为 void）
 */
template <class E>
class Unexpected {
public:
    static_assert(!std::is_void_v<E>, "Unexpected<void> is ill-formed");

    constexpr Unexpected(const Unexpected&) = default;
    constexpr Unexpected(Unexpected&&) = default;
    constexpr Unexpected& operator=(const Unexpected&) = default;
    constexpr Unexpected& operator=(Unexpected&&) = default;

    /** @brief 从可转为 E 的对象构造错误包装 */
    template <class Err = E,
              std::enable_if_t<
                  !std::is_same_v<std::remove_cvref_t<Err>, Unexpected> &&
                      std::is_constructible_v<E, Err>,
                  int> = 0>
    constexpr explicit Unexpected(Err&& e) noexcept(
        std::is_nothrow_constructible_v<E, Err>)
        : error_(std::forward<Err>(e)) {}

    /** @brief 取出错误对象（& / const& / && 引用限定） */
    constexpr E& error() & noexcept { return error_; }
    constexpr const E& error() const& noexcept { return error_; }
    constexpr E&& error() && noexcept { return std::move(error_); }
    constexpr const E&& error() const&& noexcept { return std::move(error_); }

private:
    E error_;  ///< 错误载荷（如 std::error_code、自定义类型等）
};

/** @brief CTAD：Unexpected(x) 自动推导为 Unexpected<decay_t<decltype(x)>> */
template <class E>
Unexpected(E) -> Unexpected<E>;

/**
 * @brief 工厂：把任意错误值包成 Unexpected
 * @note 常用写法 return unexpected(ec); 再隐式转为 Expected/Result
 */
template <class E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> unexpected(E&& e) {
    return Unexpected<std::decay_t<E>>(std::forward<E>(e));
}

namespace detail {

/** @brief 类型特征：判断是否为 Unexpected 特化 */
template <class U>
struct is_unexpected : std::false_type {};
template <class G>
struct is_unexpected<Unexpected<G>> : std::true_type {};
template <class U>
inline constexpr bool is_unexpected_v = is_unexpected<U>::value;

/**
 * @brief Expected<T,E> 的底层存储：同一缓冲区二选一存放成功值 T 或错误 E
 * @note 不提供默认构造；必须用 in_place（存 T）或 unexpect（存 E）构造
 */
template <class T, class E>
class ExpectedStorage {
public:
    ExpectedStorage() = delete;  ///< 禁止无参构造，避免未初始化缓冲

    /** @brief 在缓冲中 placement-new 构造成功值 T */
    template <class... Args>
    explicit ExpectedStorage(std::in_place_t, Args&&... args)
        : has_(true) {
        std::construct_at(std::addressof(as_value()), std::forward<Args>(args)...);
    }

    /** @brief 在缓冲中 placement-new 构造错误值 E */
    template <class... Args>
    explicit ExpectedStorage(unexpect_t, Args&&... args) : has_(false) {
        std::construct_at(std::addressof(as_error()), std::forward<Args>(args)...);
    }

    /** @brief 拷贝构造：按 other 状态拷贝 T 或 E */
    ExpectedStorage(const ExpectedStorage& other) : has_(other.has_) {
        if (other.has_) {
            std::construct_at(std::addressof(as_value()), other.as_value());
        } else {
            std::construct_at(std::addressof(as_error()), other.as_error());
        }
    }

    /** @brief 移动构造：按 other 状态移动 T 或 E */
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

    /** @brief true = 当前存的是 T；false = 当前存的是 E */
    [[nodiscard]] bool has_value() const noexcept { return has_; }

    /**
     * @brief 将 buf_ 解释为已存活的 T 对象并返回引用
     * @note 调用方须保证当前确实是成功态；std::launder 用于 placement-new 后的合法访问
     */
    T& as_value() & noexcept {
        return *std::launder(reinterpret_cast<T*>(buf_));
    }
    const T& as_value() const& noexcept {
        return *std::launder(reinterpret_cast<const T*>(buf_));
    }
    T&& as_value() && noexcept {
        return std::move(*std::launder(reinterpret_cast<T*>(buf_)));
    }

    /**
     * @brief 将 buf_ 解释为已存活的 E 对象并返回引用
     * @note 调用方须保证当前确实是失败态
     */
    E& as_error() & noexcept {
        return *std::launder(reinterpret_cast<E*>(buf_));
    }
    const E& as_error() const& noexcept {
        return *std::launder(reinterpret_cast<const E*>(buf_));
    }
    E&& as_error() && noexcept {
        return std::move(*std::launder(reinterpret_cast<E*>(buf_)));
    }

    /** @brief 按 has_ 析构当前活跃的 T 或 E */
    void destroy() noexcept {
        if (has_) {
            std::destroy_at(std::addressof(as_value()));
        } else {
            std::destroy_at(std::addressof(as_error()));
        }
    }

    /** @brief 在缓冲中构造成功值（调用前缓冲应为空闲或已 destroy） */
    void construct_value(const T& v) {
        std::construct_at(std::addressof(as_value()), v);
        has_ = true;
    }
    void construct_value(T&& v) {
        std::construct_at(std::addressof(as_value()), std::move(v));
        has_ = true;
    }
    /** @brief 在缓冲中构造错误值 */
    void construct_error(const E& e) {
        std::construct_at(std::addressof(as_error()), e);
        has_ = false;
    }
    void construct_error(E&& e) {
        std::construct_at(std::addressof(as_error()), std::move(e));
        has_ = false;
    }

    /**
     * @brief 从 other 赋值到 *this（同态直接赋；异态先 destroy 再 construct）
     * @note 供外层 Expected::operator= 使用
     */
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

    /** @brief 移动赋值版 assign_from */
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
    static constexpr std::size_t kSize =
        sizeof(T) > sizeof(E) ? sizeof(T) : sizeof(E);
    static constexpr std::size_t kAlign =
        alignof(T) > alignof(E) ? alignof(T) : alignof(E);

    alignas(kAlign) unsigned char buf_[kSize]{};  ///< 成功时放 T，失败时放 E
    bool has_;  ///< true=T，false=E
};

/**
 * @brief Expected<void,E> 的底层存储：成功无载荷，失败时才在缓冲中存 E
 * @note has_==true 表示成功（buf_ 无对象）；has_==false 表示失败（buf_ 中有 E）
 */
template <class E>
class ExpectedVoidStorage {
public:
    /** @brief 默认构造成功态 */
    ExpectedVoidStorage() noexcept : has_(true) {}

    /** @brief 构造成失败态，并 placement-new 错误 E */
    template <class... Args>
    explicit ExpectedVoidStorage(unexpect_t, Args&&... args) : has_(false) {
        std::construct_at(std::addressof(as_error()), std::forward<Args>(args)...);
    }

    /** @brief 拷贝：仅当 other 失败时拷贝 E */
    ExpectedVoidStorage(const ExpectedVoidStorage& other) : has_(other.has_) {
        if (!other.has_) {
            std::construct_at(std::addressof(as_error()), other.as_error());
        }
    }

    /** @brief 移动：仅当 other 失败时移动 E */
    ExpectedVoidStorage(ExpectedVoidStorage&& other) noexcept(
        std::is_nothrow_move_constructible_v<E>)
        : has_(other.has_) {
        if (!other.has_) {
            std::construct_at(std::addressof(as_error()),
                              std::move(other.as_error()));
        }
    }

    /** @brief 析构：仅失败态需要销毁 E */
    ~ExpectedVoidStorage() {
        if (!has_) {
            std::destroy_at(std::addressof(as_error()));
        }
    }

    ExpectedVoidStorage& operator=(const ExpectedVoidStorage&) = delete;
    ExpectedVoidStorage& operator=(ExpectedVoidStorage&&) = delete;

    /** @brief true=成功（无载荷）；false=失败（有错误） */
    [[nodiscard]] bool has_value() const noexcept { return has_; }

    /** @brief 取出错误 E 的引用（须为失败态） */
    E& as_error() & noexcept {
        return *std::launder(reinterpret_cast<E*>(buf_));
    }
    const E& as_error() const& noexcept {
        return *std::launder(reinterpret_cast<const E*>(buf_));
    }
    E&& as_error() && noexcept {
        return std::move(*std::launder(reinterpret_cast<E*>(buf_)));
    }

    /**
     * @brief 赋值：处理 成功↔成功 / 失败↔失败 / 成功→失败 / 失败→成功
     */
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

    /** @brief 移动赋值版 assign_from */
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
    alignas(E) unsigned char buf_[sizeof(E)]{};  ///< 仅失败态存放 E
    bool has_;  ///< true=成功，false=失败
};

}  // namespace detail

/**
 * @brief 要么持有成功值 T，要么持有错误 E（同一时间仅一种状态）
 * @tparam T 成功值类型
 * @tparam E 错误类型（不可为 void；void 成功见 Expected<void,E>）
 *
 * 日常推荐：
 *   - 判断：has_value() / if (r)
 *   - 取值：*r / value_or；少用 value()（无值会抛）
 *   - 链式：and_then / transform / or_else / transform_error
 */
template <class T, class E>
class Expected {
    static_assert(!std::is_same_v<std::remove_cv_t<E>, void>);
    static_assert(!std::is_same_v<std::remove_cv_t<T>, unexpect_t>);
    static_assert(!std::is_same_v<std::remove_cv_t<T>, std::in_place_t>);

public:
    using value_type = T;
    using error_type = E;
    using unexpected_type = Unexpected<E>;

    /** @brief 默认构造成功态（要求 T 可默认构造） */
    template <class U = T,
              std::enable_if_t<std::is_default_constructible_v<U>, int> = 0>
    Expected() : storage_(std::in_place) {}

    Expected(const Expected&) = default;
    Expected(Expected&&) = default;

    /**
     * @brief 从可转为 T 的值构造成功态
     * @param v 成功值
     */
    template <class U = T,
              std::enable_if_t<
                  !std::is_same_v<std::remove_cvref_t<U>, Expected> &&
                      !std::is_same_v<std::remove_cvref_t<U>, std::in_place_t> &&
                      !detail::is_unexpected_v<std::remove_cvref_t<U>> &&
                      std::is_constructible_v<T, U>,
                  int> = 0>
    Expected(U&& v) : storage_(std::in_place, std::forward<U>(v)) {}

    /** @brief 从 Unexpected 构造失败态（拷贝错误） */
    template <class G,
              std::enable_if_t<std::is_constructible_v<E, const G&>, int> = 0>
    Expected(const Unexpected<G>& u) : storage_(unexpect, u.error()) {}

    /** @brief 从 Unexpected 构造失败态（移动错误） */
    template <class G, std::enable_if_t<std::is_constructible_v<E, G>, int> = 0>
    Expected(Unexpected<G>&& u) : storage_(unexpect, std::move(u.error())) {}

    /** @brief 原位构造成功值 T(args...) */
    template <class... Args,
              std::enable_if_t<std::is_constructible_v<T, Args...>, int> = 0>
    explicit Expected(std::in_place_t, Args&&... args)
        : storage_(std::in_place, std::forward<Args>(args)...) {}

    /** @brief 原位构造错误值 E(args...) */
    template <class... Args,
              std::enable_if_t<std::is_constructible_v<E, Args...>, int> = 0>
    explicit Expected(unexpect_t, Args&&... args)
        : storage_(unexpect, std::forward<Args>(args)...) {}

    /** @brief 拷贝赋值 */
    Expected& operator=(const Expected& other) {
        if (this != &other) {
            storage_.assign_from(other.storage_);
        }
        return *this;
    }

    /** @brief 移动赋值 */
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

    /** @brief 是否处于成功态 */
    [[nodiscard]] bool has_value() const noexcept { return storage_.has_value(); }
    /** @brief 同 has_value()，便于写 if (r) */
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    /**
     * @brief 取成功值；无值时抛 BadExpectedAccess（兜底，非推荐主路径）
     * @note 日常请先 if (r) 或用 value_or / *
     */
    T& value() & {
        if (!has_value()) {
            throw BadExpectedAccess{};
        }
        return storage_.as_value();
    }
    /** @brief const 版 value() */
    const T& value() const& {
        if (!has_value()) {
            throw BadExpectedAccess{};
        }
        return storage_.as_value();
    }
    /** @brief 右值版 value()，可移动取出成功值 */
    T&& value() && {
        if (!has_value()) {
            throw BadExpectedAccess{};
        }
        return std::move(storage_.as_value());
    }

    /**
     * @brief 取错误；调用方须保证当前为失败态（成功时 assert 失败）
     * @return 错误对象引用
     */
    E& error() & {
        assert(!has_value());
        return storage_.as_error();
    }
    /** @brief const 版 error() */
    const E& error() const& {
        assert(!has_value());
        return storage_.as_error();
    }
    /** @brief 右值版 error()，可移动取出错误 */
    E&& error() && {
        assert(!has_value());
        return std::move(storage_.as_error());
    }

    /**
     * @brief 有值返回值，否则返回默认值 def（不抛异常）
     * @tparam U 可转为 T 的默认值类型
     */
    template <class U>
    T value_or(U&& def) const& {
        return has_value() ? storage_.as_value()
                           : static_cast<T>(std::forward<U>(def));
    }
    /** @brief 右值版 value_or：成功时可移动值 */
    template <class U>
    T value_or(U&& def) && {
        return has_value() ? std::move(storage_.as_value())
                           : static_cast<T>(std::forward<U>(def));
    }

    /**
     * @brief 有错误返回错误，否则返回默认错误 def（不抛异常）
     * @tparam G 可转为 E 的默认值类型（默认 G=E）
     */
    template <class G = E>
    E error_or(G&& def) const& {
        return !has_value() ? storage_.as_error()
                            : static_cast<E>(std::forward<G>(def));
    }

    /**
     * @brief 解引用取成功值（不检查；调用方须保证有值）
     */
    T& operator*() & noexcept { return storage_.as_value(); }
    const T& operator*() const& noexcept { return storage_.as_value(); }
    T&& operator*() && noexcept { return std::move(storage_.as_value()); }

    /** @brief 成员访问成功值（不检查；调用方须保证有值） */
    T* operator->() noexcept { return std::addressof(storage_.as_value()); }
    const T* operator->() const noexcept {
        return std::addressof(storage_.as_value());
    }

    /**
     * @brief 成功则调用 f(值)，f 须再返回 Expected/Result；失败则错误原样传递（短路）
     * @param f 接受 T& 并返回某种 Expected 的可调用对象
     */
    template <class F>
    auto and_then(F&& f) & {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T&>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f), storage_.as_value());
        }
        return U(unexpect, storage_.as_error());
    }
    /** @brief const 版 and_then */
    template <class F>
    auto and_then(F&& f) const& {
        using U = std::remove_cvref_t<std::invoke_result_t<F, const T&>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f), storage_.as_value());
        }
        return U(unexpect, storage_.as_error());
    }
    /** @brief 右值版 and_then（可移动值/错误） */
    template <class F>
    auto and_then(F&& f) && {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T&&>>;
        if (has_value()) {
            return std::invoke(std::forward<F>(f), std::move(storage_.as_value()));
        }
        return U(unexpect, std::move(storage_.as_error()));
    }

    /**
     * @brief 成功则用 f 映射值（f 返回普通值或 void）；失败则错误原样传递
     * @param f 接受 T& 的映射函数
     */
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
    /** @brief const 版 transform */
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
    /** @brief 右值版 transform */
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

    /**
     * @brief 失败才调用 f(错误) 做补救；成功则把原值包进 f 的返回类型并返回
     * @param f 接受 E& 并返回某种 Expected 的可调用对象
     */
    template <class F>
    auto or_else(F&& f) & {
        using G = std::remove_cvref_t<std::invoke_result_t<F, E&>>;
        if (has_value()) {
            return G(std::in_place, storage_.as_value());
        }
        return std::invoke(std::forward<F>(f), storage_.as_error());
    }
    /** @brief const 版 or_else */
    template <class F>
    auto or_else(F&& f) const& {
        using G = std::remove_cvref_t<std::invoke_result_t<F, const E&>>;
        if (has_value()) {
            return G(std::in_place, storage_.as_value());
        }
        return std::invoke(std::forward<F>(f), storage_.as_error());
    }
    /** @brief 右值版 or_else */
    template <class F>
    auto or_else(F&& f) && {
        using G = std::remove_cvref_t<std::invoke_result_t<F, E&&>>;
        if (has_value()) {
            return G(std::in_place, std::move(storage_.as_value()));
        }
        return std::invoke(std::forward<F>(f), std::move(storage_.as_error()));
    }

    /**
     * @brief 失败时用 f 映射错误类型/内容；成功则值原样保留
     * @param f 接受 E&，返回新的错误类型 G
     * @return Expected<T, G>
     */
    template <class F>
    auto transform_error(F&& f) & {
        using G = std::remove_cv_t<std::invoke_result_t<F, E&>>;
        using R = Expected<T, G>;
        if (has_value()) {
            return R(std::in_place, storage_.as_value());
        }
        return R(unexpect, std::invoke(std::forward<F>(f), storage_.as_error()));
    }
    /** @brief const 版 transform_error */
    template <class F>
    auto transform_error(F&& f) const& {
        using G = std::remove_cv_t<std::invoke_result_t<F, const E&>>;
        using R = Expected<T, G>;
        if (has_value()) {
            return R(std::in_place, storage_.as_value());
        }
        return R(unexpect, std::invoke(std::forward<F>(f), storage_.as_error()));
    }
    /** @brief 右值版 transform_error */
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
    detail::ExpectedStorage<T, E> storage_;  ///< 实际存放 T 或 E
};

/**
 * @brief Expected 的 void 特化：成功无载荷，失败只携带错误 E
 * @tparam E 错误类型
 * @note 适合“只关心成没成功”的操作，如 ensure_ready() -> Result<void>
 */
template <class E>
class Expected<void, E> {
    static_assert(!std::is_same_v<std::remove_cv_t<E>, void>);

public:
    using value_type = void;
    using error_type = E;
    using unexpected_type = Unexpected<E>;

    /** @brief 默认构造成功态 */
    Expected() noexcept = default;
    Expected(const Expected&) = default;
    Expected(Expected&&) = default;

    /** @brief 从 Unexpected 构造失败态 */
    template <class G,
              std::enable_if_t<std::is_constructible_v<E, const G&>, int> = 0>
    Expected(const Unexpected<G>& u) : storage_(unexpect, u.error()) {}

    template <class G, std::enable_if_t<std::is_constructible_v<E, G>, int> = 0>
    Expected(Unexpected<G>&& u) : storage_(unexpect, std::move(u.error())) {}

    /** @brief 显式标记成功（无载荷） */
    explicit Expected(std::in_place_t) noexcept : storage_() {}

    /** @brief 原位构造错误 E(args...) */
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

    /** @brief 是否成功 */
    [[nodiscard]] bool has_value() const noexcept { return storage_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    /**
     * @brief 成功则无操作；失败抛 BadExpectedAccess
     * @note void 特化无返回值，仅用于检查
     */
    void value() const {
        if (!has_value()) {
            throw BadExpectedAccess{};
        }
    }

    /** @brief 取错误（须为失败态） */
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

    /** @brief 有错误返回错误，否则返回 def */
    template <class G = E>
    E error_or(G&& def) const& {
        return !has_value() ? storage_.as_error()
                            : static_cast<E>(std::forward<G>(def));
    }

    /** @brief void 成功态解引用为空操作 */
    void operator*() const noexcept {}

    /**
     * @brief 成功则调用无参 f()；失败则错误原样传递
     * @note f 不接收值（因为成功无载荷）
     */
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

    /**
     * @brief 成功则调用无参 f 并映射结果；失败则错误原样传递
     */
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

    /** @brief 失败才调用 f(错误)；成功返回空成功的 G{} */
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

    /**
     * @brief 失败时映射错误；成功保持 Expected<void, G> 成功态
     */
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

// ---------------------------------------------------------------------------
// 工厂函数与 Result 别名
// ---------------------------------------------------------------------------

/**
 * @brief 构造成功的 Expected<decay_t<T>, E>
 * @param value 成功值
 */
template <class T, class E = std::error_code>
[[nodiscard]] Expected<std::decay_t<T>, E> ok(T&& value) {
    return Expected<std::decay_t<T>, E>(std::in_place, std::forward<T>(value));
}

/**
 * @brief 构造成功的 Expected<void, E>（无载荷）
 */
template <class E = std::error_code>
[[nodiscard]] Expected<void, E> ok() {
    return Expected<void, E>(std::in_place);
}

/**
 * @brief 构造 Unexpected（再赋给 Expected/Result 即失败态）
 * @param e 错误对象
 */
template <class E>
[[nodiscard]] auto err(E&& e) {
    return unexpected(std::forward<E>(e));
}

/**
 * @brief 常用别名：错误类型固定为 std::error_code
 */
template <class T>
using Result = Expected<T, std::error_code>;

/** @brief 成功的 Result<void> */
[[nodiscard]] inline Result<void> result_ok() { return ok<>(); }

/**
 * @brief 成功的 Result<T>
 * @param value 成功值
 */
template <class T>
[[nodiscard]] inline Result<std::decay_t<T>> result_ok(T&& value) {
    return ok<T, std::error_code>(std::forward<T>(value));
}

/**
 * @brief 失败：从 std::error_code 构造 Unexpected
 * @note 需赋给 Result/Expected 才成为完整失败结果，例如 Result<int> r = result_err(ec);
 */
[[nodiscard]] inline auto result_err(std::error_code ec) {
    return unexpected(ec);
}

/**
 * @brief 失败：从 std::errc 转为 error_code 再包装
 */
[[nodiscard]] inline auto result_err(std::errc code) {
    return unexpected(std::make_error_code(code));
}

}  // namespace utils

#pragma once

/**
 * span / string_view 薄工具层（C++20）
 *
 * 不重造 std::span / std::string_view；提供字节视图与非拥有边界辅助。
 */

#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>

namespace utils {

using byte_span = std::span<std::byte>;
using const_byte_span = std::span<const std::byte>;

using std::as_bytes;
using std::as_writable_bytes;

template <class C>
    requires std::ranges::contiguous_range<C> && std::ranges::sized_range<C>
[[nodiscard]] constexpr auto as_span(C& c) noexcept {
    using T = std::remove_reference_t<std::ranges::range_reference_t<C>>;
    return std::span<T>(std::ranges::data(c), std::ranges::size(c));
}

template <class C>
    requires std::ranges::contiguous_range<const C> &&
             std::ranges::sized_range<const C>
[[nodiscard]] constexpr auto as_span(const C& c) noexcept {
    using T = std::remove_reference_t<std::ranges::range_reference_t<const C>>;
    return std::span<T>(std::ranges::data(c), std::ranges::size(c));
}

template <class T, std::size_t N>
[[nodiscard]] constexpr std::span<T> as_span(T (&arr)[N]) noexcept {
    return std::span<T>(arr, N);
}

template <class T>
[[nodiscard]] constexpr std::span<T> as_span(T* ptr, std::size_t n) noexcept {
    return std::span<T>(ptr, n);
}

[[nodiscard]] constexpr std::string_view trim_left(std::string_view sv) noexcept {
    std::size_t i = 0;
    while (i < sv.size() &&
           (sv[i] == ' ' || sv[i] == '\t' || sv[i] == '\n' || sv[i] == '\r')) {
        ++i;
    }
    return sv.substr(i);
}

[[nodiscard]] constexpr std::string_view trim_right(std::string_view sv) noexcept {
    std::size_t n = sv.size();
    while (n > 0 && (sv[n - 1] == ' ' || sv[n - 1] == '\t' ||
                     sv[n - 1] == '\n' || sv[n - 1] == '\r')) {
        --n;
    }
    return sv.substr(0, n);
}

[[nodiscard]] constexpr std::string_view trim(std::string_view sv) noexcept {
    return trim_right(trim_left(sv));
}

/** 惰性按分隔符切分；不分配。 */
class split_view {
public:
    class iterator {
    public:
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;

        iterator() = default;
        iterator(std::string_view data, char delim, bool end)
            : data_(data), delim_(delim), end_(end) {
            if (!end_) {
                advance();
            }
        }

        std::string_view operator*() const noexcept { return current_; }

        iterator& operator++() {
            advance();
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        friend bool operator==(const iterator& a, const iterator& b) noexcept {
            if (a.end_ && b.end_) {
                return true;
            }
            return a.end_ == b.end_ && a.pos_ == b.pos_ &&
                   a.data_.data() == b.data_.data() && a.data_.size() == b.data_.size();
        }

    private:
        void advance() {
            if (pos_ > data_.size()) {
                end_ = true;
                current_ = {};
                return;
            }
            if (pos_ == data_.size()) {
                // 末尾空段（若以分隔符结尾）
                current_ = data_.substr(pos_, 0);
                ++pos_;
                end_ = true;
                return;
            }
            const auto rest = data_.substr(pos_);
            const auto found = rest.find(delim_);
            if (found == std::string_view::npos) {
                current_ = rest;
                pos_ = data_.size() + 1;
            } else {
                current_ = rest.substr(0, found);
                pos_ += found + 1;
            }
        }

        std::string_view data_{};
        std::string_view current_{};
        std::size_t pos_ = 0;
        char delim_ = ',';
        bool end_ = true;
    };

    constexpr split_view(std::string_view data, char delim) noexcept
        : data_(data), delim_(delim) {}

    [[nodiscard]] iterator begin() const {
        return iterator(data_, delim_, /*end=*/false);
    }
    [[nodiscard]] iterator end() const {
        return iterator(data_, delim_, true);
    }

private:
    std::string_view data_;
    char delim_;
};

[[nodiscard]] inline split_view split(std::string_view sv, char delim) noexcept {
    return split_view(sv, delim);
}

}  // namespace utils

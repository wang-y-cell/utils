#pragma once

/**
 * 字节字面量（1024 进制），便于配置 size_class bands / block_size。
 *
 *   using namespace utils::byte_literals;
 *   auto cfg = size_class_config::from_bands({{8, 128}, {128, 4_KB}});
 *   memory_pool pool(1_MB);
 *
 * 注意：bands 的 step 仍须为 2 的幂；3_KB 可作 max，不可作 step。
 */

#include <cstddef>

namespace utils {
inline namespace byte_literals {

[[nodiscard]] constexpr std::size_t operator""_KB(
    unsigned long long v) noexcept {
    return static_cast<std::size_t>(v * 1024ULL);
}

[[nodiscard]] constexpr std::size_t operator""_MB(
    unsigned long long v) noexcept {
    return static_cast<std::size_t>(v * 1024ULL * 1024ULL);
}

[[nodiscard]] constexpr std::size_t operator""_GB(
    unsigned long long v) noexcept {
    return static_cast<std::size_t>(v * 1024ULL * 1024ULL * 1024ULL);
}

[[nodiscard]] constexpr std::size_t operator""_TB(
    unsigned long long v) noexcept {
    return static_cast<std::size_t>(v * 1024ULL * 1024ULL * 1024ULL *
                                    1024ULL);
}

}  // namespace byte_literals
}  // namespace utils

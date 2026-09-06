#pragma once

/**
 * 轻量运行时计时 / 微基准工具（来自 main 中的测速脚手架）。
 *
 *   #include "Tools/runtime_test.h"
 *
 * 典型用法：
 *   using namespace utils::bench;
 *   const auto us = time_us([&] { ... });
 *   print_row("name", us);
 *   // 或一步完成：
 *   run("name", [&] { ... });
 */

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace utils {
namespace bench {

/** 按最大合适单位格式化字节：B / KB / MB / GB（1024 进制） */
[[nodiscard]] inline std::string bytes_human(std::size_t n) {
    char buf[64];
    if (n >= (std::size_t{1} << 30)) {
        std::snprintf(buf, sizeof(buf), "%.3f GB",
                      static_cast<double>(n) / 1073741824.0);
    } else if (n >= (std::size_t{1} << 20)) {
        std::snprintf(buf, sizeof(buf), "%.3f MB",
                      static_cast<double>(n) / 1048576.0);
    } else if (n >= (std::size_t{1} << 10)) {
        std::snprintf(buf, sizeof(buf), "%.3f KB",
                      static_cast<double>(n) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%zu B", n);
    }
    return buf;
}

/** 阻止编译器把「未使用」的指针/分配优化掉（测速用） */
inline void escape(void* p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(p) : "memory");
#else
    static void* volatile sink;
    sink = p;
#endif
}

/** 对任意值做同样的防优化（按值/引用均可） */
template <class T>
inline void escape(T&& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(std::addressof(value)) : "memory");
#else
    static typename std::remove_reference<T>::type* volatile sink;
    sink = std::addressof(value);
#endif
}

/** 执行 f，返回耗时（微秒） */
template <class F>
[[nodiscard]] long long time_us(F&& f) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    std::forward<F>(f)();
    const auto t1 = clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
        .count();
}

/** 执行 f，返回耗时（毫秒，浮点） */
template <class F>
[[nodiscard]] double time_ms(F&& f) {
    return time_us(std::forward<F>(f)) / 1000.0;
}

/** 打印一行：name: N us (M ms) */
inline void print_row(const char* name, long long us) {
    std::cout << "  " << name << ": " << us << " us (" << (us / 1000.0)
              << " ms)\n";
}

/** 打印分组标题 */
inline void print_title(const char* title) {
    std::cout << title << '\n';
}

/**
 * 计时并打印；返回微秒。
 * print=false 时只计时不输出。
 */
template <class F>
long long run(const char* name, F&& f, bool print = true) {
    const long long us = time_us(std::forward<F>(f));
    if (print) {
        print_row(name, us);
    }
    return us;
}

}  // namespace bench
}  // namespace utils

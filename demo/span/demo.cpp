/**
 * span_utils 用法演示
 * 编译: cmake --build build --target demo_span
 *
 * 要点:
 * - as_span: 容器/数组/指针 → std::span（不拥有）
 * - byte_span / as_bytes: 字节视图，方便对接二进制接口
 * - trim / split: 基于 string_view，不分配
 */

#include "span/span_utils.h"

#include <iostream>
#include <string>
#include <vector>

using namespace utils;

static void process(const_byte_span bytes) {
    std::cout << "  process bytes=" << bytes.size() << '\n';
}

int main() {
    std::cout << "=== 1) as_span ===\n";
    std::vector<int> v{10, 20, 30};
    auto s = as_span(v);
    std::cout << "  size=" << s.size() << " s[1]=" << s[1] << '\n';

    int arr[] = {1, 2};
    auto sa = as_span(arr);
    std::cout << "  array span size=" << sa.size() << '\n';

    std::cout << "\n=== 2) 字节视图 ===\n";
    process(as_bytes(s));

    std::cout << "\n=== 3) trim / split ===\n";
    std::cout << "  trim='" << trim("  hi \n") << "'\n";
    std::cout << "  split: ";
    for (auto part : split("a,b,c", ',')) {
        std::cout << '[' << part << ']';
    }
    std::cout << '\n';

    std::cout << "\ndemo_span: ok\n";
    return 0;
}

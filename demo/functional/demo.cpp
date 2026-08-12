/**
 * function_ref / any_invocable 用法演示
 * 编译: cmake --build build --target demo_functional
 *
 * 要点:
 * - function_ref: 非拥有，适合函数参数（类似 string_view）
 * - any_invocable: 拥有且只移动，可存 unique_ptr 捕获；相对 std::function 不要求可拷贝
 */

#include "functional/any_invocable.h"
#include "functional/function_ref.h"

#include <functional>
#include <iostream>
#include <memory>
#include <vector>

using namespace utils;

static int add(int a, int b) { return a + b; }

static void apply_each(const std::vector<int>& v,
                       function_ref<void(int)> visitor) {
    for (int x : v) {
        visitor(x);
    }
}

int main() {
    std::cout << "=== 1) function_ref：参数传递 ===\n";
    auto print = [](int x) { std::cout << "  item=" << x << '\n'; };
    apply_each({1, 2, 3}, print);

    function_ref<int(int, int)> fp = add;
    std::cout << "  add via ref=" << fp(20, 22) << '\n';

    std::cout << "\n=== 2) any_invocable：可移动、可存 unique_ptr ===\n";
    any_invocable<int()> task = [p = std::make_unique<int>(40)] {
        return *p + 2;
    };
    std::cout << "  task()=" << task() << '\n';

    // 空调用抛 bad_function_call
    any_invocable<void()> empty;
    try {
        empty();
    } catch (const std::bad_function_call&) {
        std::cout << "  empty threw bad_function_call\n";
    }

    std::function<void()> sf = [] { std::cout << "  from std::function\n"; };
    any_invocable<void()> moved = std::move(sf);
    moved();

    std::cout << "\ndemo_functional: ok\n";
    return 0;
}

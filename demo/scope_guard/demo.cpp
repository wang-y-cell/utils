/**
 * scope_guard 用法演示
 * 编译: cmake --build build --target demo_scope_guard
 *
 * 要点:
 * - make_scope_guard / UTILS_DEFER: 离开作用域必执行
 * - scope_fail: 仅异常展开时执行（回滚）
 * - scope_success: 仅正常离开时执行
 * - dismiss(): 取消清理（例如所有权已移交）
 */

#include "scope_guard/scope_guard.h"

#include <iostream>
#include <stdexcept>

using namespace utils;

int main() {
    std::cout << "=== 1) 普通守卫：任意离开都清理 ===\n";
    int closed = 0;
    {
        auto guard = make_scope_guard([&] {
            ++closed;
            std::cout << "  fclose/cleanup\n";
        });
        std::cout << "  working...\n";
        // 提前 return / 离开块都会触发
    }
    std::cout << "  closed=" << closed << '\n';

    std::cout << "\n=== 2) dismiss：成功后不再清理 ===\n";
    {
        auto guard = make_scope_guard([&] {
            std::cout << "  should NOT run\n";
        });
        std::cout << "  transfer ownership -> dismiss\n";
        guard.dismiss();
    }

    std::cout << "\n=== 3) UTILS_DEFER 语法糖 ===\n";
    {
        UTILS_DEFER { std::cout << "  defer ran\n"; };
        std::cout << "  before leave\n";
    }

    std::cout << "\n=== 4) scope_fail / scope_success ===\n";
    try {
        scope_fail rollback{[] { std::cout << "  rollback (exception path)\n"; }};
        scope_success commit{[] { std::cout << "  commit (success path)\n"; }};
        throw std::runtime_error("boom");
    } catch (...) {
        std::cout << "  caught\n";
    }
    {
        scope_fail rollback{[] { std::cout << "  rollback should NOT run\n"; }};
        scope_success commit{[] { std::cout << "  commit ran\n"; }};
    }

    std::cout << "\ndemo_scope_guard: ok\n";
    return 0;
}

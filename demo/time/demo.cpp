/**
 * stop_watch / deadline 用法演示
 * 编译: cmake --build build --target demo_deadline
 *
 * 要点:
 * - deadline::after / remaining / expired：截止时间语义
 * - stop_watch：测量耗时（调试、指标）
 * - 常与 channel::recv 超时逻辑、retry、std::stop_token 组合
 */

#include "component/time/deadline.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

int main() {
    std::cout << "=== 1) deadline ===\n";
    auto d = deadline::after(40ms);
    std::cout << "  expired? " << d.expired() << '\n';
    int loops = 0;
    while (!d.expired()) {
        ++loops;
        std::this_thread::sleep_for(5ms);
    }
    std::cout << "  loops=" << loops
              << " remaining_ms=" << d.remaining_as<std::chrono::milliseconds>().count()
              << '\n';

    auto never = deadline::never();
    std::cout << "  never.expired=" << never.expired() << '\n';

    std::cout << "\n=== 2) stop_watch ===\n";
    stop_watch sw;
    std::this_thread::sleep_for(20ms);
    std::cout << "  elapsed_ms=" << sw.elapsed_ms()
              << " elapsed_us=" << sw.elapsed_us() << '\n';
    sw.reset();
    std::this_thread::sleep_for(5ms);
    std::cout << "  after reset ms=" << sw.elapsed_ms() << '\n';

    std::cout << "\ndemo_deadline: ok\n";
    return 0;
}

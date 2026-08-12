/**
 * CancellationToken 用法演示
 * 编译: cmake --build build --target demo_cancel
 *
 * 要点:
 * - make_cancellation() 得到 token + source
 * - 任务轮询 token.stop_requested()（协作取消，不杀线程）
 * - 可与 thread_pool / retry / 循环配合
 */

#include "cancel/cancellation.h"
#include "thread_pool/thread_pool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

int main() {
    std::cout << "=== 1) 基本 request_stop ===\n";
    auto [token, source] = make_cancellation();
    std::cout << "  before=" << token.stop_requested() << '\n';
    source.request_stop();
    std::cout << "  after=" << token.stop_requested() << '\n';

    std::cout << "\n=== 2) 工作循环中响应取消 ===\n";
    auto pair = make_cancellation();
    thread_pool pool(2);
    std::atomic<int> ticks{0};

    pool.add_task([token = pair.token, &ticks] {
        while (!token.stop_requested()) {
            ++ticks;
            std::this_thread::sleep_for(5ms);
        }
        std::cout << "  worker stopped, ticks=" << ticks.load() << '\n';
    });

    std::this_thread::sleep_for(25ms);
    pair.source.request_stop();
    pool.wait();

    std::cout << "\ndemo_cancel: ok\n";
    return 0;
}

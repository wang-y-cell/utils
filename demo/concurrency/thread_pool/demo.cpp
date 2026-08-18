/**
 * thread_pool 用法演示
 * 编译: cmake --build build --target demo_thread_pool
 *
 * 要点:
 * - submit: 返回 future，异常在 get() 时抛出
 * - add_task / try_add_task: fire-and-forget；后者队列满不阻塞
 * - 有界队列背压；wait / shutdown / resize
 */

#include "concurrency/thread_pool/thread_pool.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

int main() {
    std::cout << "=== 1) submit + future ===\n";
    thread_pool pool(4, /*max_queue_size=*/32);

    auto fut = pool.submit([] { return 40 + 2; });
    std::cout << "  result=" << fut.get() << '\n';

    auto bad = pool.submit([]() -> int {
        throw std::runtime_error("boom");
    });
    try {
        bad.get();
    } catch (const std::exception& e) {
        std::cout << "  future exception: " << e.what() << '\n';
    }

    std::cout << "\n=== 2) add_task ===\n";
    for (int i = 0; i < 4; ++i) {
        pool.add_task([i] {
            std::this_thread::sleep_for(5ms);
            std::cout << "  task " << i << " done\n";
        });
    }
    pool.wait();

    std::cout << "\n=== 3) try_add_task / resize ===\n";
    thread_pool tiny(1, /*max_queue_size=*/1);
    tiny.add_task([] { std::this_thread::sleep_for(30ms); });
    // 队列可能已满：try 不阻塞
    bool accepted = tiny.try_add_task([] {});
    std::cout << "  try_add_task accepted=" << accepted << '\n';
    tiny.resize(2);
    std::cout << "  target threads=" << tiny.target_thread_count() << '\n';
    tiny.wait();
    tiny.shutdown();

    std::cout << "\ndemo_thread_pool: ok\n";
    return 0;
}

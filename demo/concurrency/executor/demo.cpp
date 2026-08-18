/**
 * executor 用法演示
 * 编译: cmake --build build --target demo_executor
 *
 * 要点:
 * - concept executor: 只要能 post(any_invocable<void()>)
 * - inline_executor: 当前线程同步执行（测试默认）
 * - make_executor(thread_pool/event_loop): 适配现有执行器
 * - any_executor: 类型擦除，运行时可替换后端
 */

#include "concurrency/executor/adapters.h"
#include "concurrency/executor/executor.h"
#include "concurrency/thread_pool/thread_pool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace utils;

int main() {
    std::cout << "=== 1) inline_executor ===\n";
    inline_executor inline_ex;
    int x = 0;
    inline_ex.post([&] { x = 42; });
    std::cout << "  x=" << x << '\n';

    std::cout << "\n=== 2) thread_pool 适配 ===\n";
    thread_pool pool(2);
    auto pool_ex = make_executor(pool);
    std::atomic<int> y{0};
    pool_ex.post([&] { y = 1; });
    any_invocable<void()> move_only = [&] { y = 2; };
    pool_ex.post(std::move(move_only));
    pool.wait();
    std::cout << "  y=" << y.load() << '\n';

    std::cout << "\n=== 3) any_executor 类型擦除 ===\n";
    any_executor any = pool_ex;
    any.post([&] { y = 3; });
    pool.wait();
    std::cout << "  y=" << y.load() << '\n';

    inline_executor again;
    any = any_executor{again};
    any.post([&] { y = 4; });
    std::cout << "  after inline y=" << y.load() << '\n';

    std::cout << "\ndemo_executor: ok\n";
    return 0;
}

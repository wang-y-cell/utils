#include "thread_pool/thread_pool.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {
    utils::thread_pool pool(4, /*max_queue_size=*/64);

    auto fut = pool.submit([] {
        return 40 + 2;
    });
    std::cout << "submit result: " << fut.get() << '\n';

    auto bad = pool.submit([]() -> int {
        throw std::runtime_error("boom");
    });
    try {
        bad.get();
    } catch (const std::exception& e) {
        std::cout << "future exception: " << e.what() << '\n';
    }

    for (int i = 0; i < 8; ++i) {
        pool.add_task([i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::cout << "task " << i << " done\n";
        });
    }

    pool.resize(2);
    pool.wait();
    pool.shutdown();
    std::cout << "ok\n";
    return 0;
}

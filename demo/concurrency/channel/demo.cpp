/**
 * channel 用法演示
 * 编译: cmake --build build --target demo_channel
 *
 * 要点:
 * - 有界队列背压；capacity==0 无界
 * - send/recv 阻塞；try_send/try_recv 非阻塞
 * - close 后 send 失败；recv 排空后返回 nullopt
 * - 适合数据流；事件回调请用 signal_and_slots
 */

#include "concurrency/channel/channel.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

int main() {
    std::cout << "=== 1) 有界队列 ===\n";
    channel<int> ch(2);
    std::cout << "  send 1/2: " << ch.send(1) << ' ' << ch.send(2) << '\n';
    std::cout << "  try_send when full: " << ch.try_send(3) << '\n';
    std::cout << "  recv=" << ch.recv().value() << '\n';

    std::cout << "\n=== 2) 多生产者 → 单消费者 ===\n";
    channel<int> pipe(0);  // 无界，避免生产者在 join 前堵满
    std::thread p1([&] {
        for (int i = 0; i < 5; ++i) {
            pipe.send(i);
        }
    });
    std::thread p2([&] {
        for (int i = 100; i < 105; ++i) {
            pipe.send(i);
        }
    });
    p1.join();
    p2.join();
    pipe.close();

    int count = 0;
    int sum = 0;
    while (auto v = pipe.recv()) {
        sum += *v;
        ++count;
    }
    std::cout << "  count=" << count << " sum=" << sum << '\n';

    std::cout << "\ndemo_channel: ok\n";
    return 0;
}

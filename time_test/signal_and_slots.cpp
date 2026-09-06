/**
 * signal_and_slots 微基准（time_test）
 *
 *   cmake --build build --target time_signal
 *   ./build/time_signal   # Windows: build\time_signal.exe
 *
 * 对比：裸调用 / std::function / Direct emit / 多槽 / Queued（同线程与跨线程）
 */

#include "Tools/runtime_test.h"
#include "concurrency/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

using utils::bench::escape;
using utils::bench::print_title;
using utils::bench::time_us;
using utils::connection_type;
using utils::core_application;
using utils::object;
using utils::scoped_connection;
using utils::slots_t;
using utils::worker_thread;

namespace {

constexpr int N = 200000;

class counter : public object {
public:
    ~counter() override { invalidate(); }

    slots_t<> on_hit() {
        ++hits;
        return {};
    }

    slots_t<> on_hit_arg(int v) {
        hits += v;
        escape(&hits);
        return {};
    }

    std::uint64_t hits = 0;
};

class emitter : public object {
public:
    utils::signal<> ping{this};
    utils::signal<int> ping_int{this};
};

void print_ns_per(const char* name, long long us, int n) {
    const double ns = (n > 0) ? (us * 1000.0 / n) : 0.0;
    std::cout << "  " << name << ": " << us << " us (" << (us / 1000.0)
              << " ms)  ~" << ns << " ns/op\n";
}

}  // namespace

int main() {
    core_application app;

    // ---------- 1) 同线程 Direct：相对裸调用 ----------
    {
        std::uint64_t sink = 0;
        const auto raw = time_us([&] {
            for (int i = 0; i < N; ++i) {
                ++sink;
                escape(&sink);
            }
        });

        std::function<void()> fn = [&] {
            ++sink;
            escape(&sink);
        };
        const auto stdfn = time_us([&] {
            for (int i = 0; i < N; ++i) {
                fn();
            }
        });

        emitter src;
        counter dst;
        scoped_connection c{
            utils::connect(src.ping, &dst, &counter::on_hit,
                           connection_type::direct)};
        dst.hits = 0;
        const auto direct1 = time_us([&] {
            for (int i = 0; i < N; ++i) {
                src.ping.emit();
            }
        });
        escape(&dst.hits);

        print_title("same-thread Direct  N=200000  (1 slot vs baselines)");
        print_ns_per("raw ++", raw, N);
        print_ns_per("std::function", stdfn, N);
        print_ns_per("signal Direct 1 slot", direct1, N);
        std::cout << "  (hits=" << dst.hits << ")\n\n";
    }

    // ---------- 2) Direct：0 / 1 / 8 槽 ----------
    {
        emitter src8;
        counter slots[8];
        std::vector<scoped_connection> cons;
        cons.reserve(8);
        for (auto& d : slots) {
            cons.emplace_back(utils::connect(src8.ping, &d, &counter::on_hit,
                                             connection_type::direct));
            d.hits = 0;
        }
        const auto eight = time_us([&] {
            for (int i = 0; i < N; ++i) {
                src8.ping.emit();
            }
        });

        emitter src1;
        counter one;
        scoped_connection c1{utils::connect(src1.ping, &one, &counter::on_hit,
                                            connection_type::direct)};
        one.hits = 0;
        const auto one_t = time_us([&] {
            for (int i = 0; i < N; ++i) {
                src1.ping.emit();
            }
        });

        emitter bare;
        const auto zero = time_us([&] {
            for (int i = 0; i < N; ++i) {
                bare.ping.emit();
            }
        });

        print_title("same-thread Direct  N=200000  (0 / 1 / 8 slots)");
        print_ns_per("0 slots", zero, N);
        print_ns_per("1 slot", one_t, N);
        print_ns_per("8 slots", eight, N);
        std::cout << '\n';
    }

    // ---------- 3) 同线程 Queued：emit 全部入队再排空 ----------
    {
        emitter src;
        counter dst;
        // 与 src 同线程；强制 Queued
        scoped_connection c{utils::connect(src.ping, &dst, &counter::on_hit,
                                           connection_type::queued)};
        dst.hits = 0;

        auto* loop = dst.loop();
        const auto queued = time_us([&] {
            for (int i = 0; i < N; ++i) {
                src.ping.emit();
            }
            if (loop) {
                loop->process_events();
            }
        });

        print_title(
            "same-thread Queued  N=200000  (emit all then process_events)");
        print_ns_per("Queued emit+drain", queued, N);
        std::cout << "  (hits=" << dst.hits << ")\n\n";
    }

    // ---------- 4) 带 int 参数 Direct ----------
    {
        emitter src;
        counter dst;
        scoped_connection c{utils::connect(src.ping_int, &dst,
                                           &counter::on_hit_arg,
                                           connection_type::direct)};
        dst.hits = 0;
        const auto with_arg = time_us([&] {
            for (int i = 0; i < N; ++i) {
                src.ping_int.emit(1);
            }
        });
        print_title("same-thread Direct  N=200000  (signal<int>)");
        print_ns_per("Direct 1 slot + int", with_arg, N);
        std::cout << "  (hits=" << dst.hits << ")\n\n";
    }

    // ---------- 5) 跨线程 Queued（较少迭代，含等待） ----------
    {
        constexpr int N_x = 20000;
        worker_thread worker;
        worker.start();

        emitter src;
        counter dst;
        dst.move_to_thread(worker);
        scoped_connection c{utils::connect(src.ping, &dst, &counter::on_hit,
                                           connection_type::queued)};

        // 等 worker 进入泵送
        for (int i = 0; i < 200 && !worker.loop()->is_pumping(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        dst.hits = 0;
        const auto cross = time_us([&] {
            for (int i = 0; i < N_x; ++i) {
                src.ping.emit();
            }
            // 等槽跑完
            while (dst.hits < static_cast<std::uint64_t>(N_x)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

        print_title(
            "cross-thread Queued  N=20000  (emit + wait worker drain)");
        print_ns_per("Queued cross-thread", cross, N_x);
        std::cout << "  (hits=" << dst.hits << ")\n";

        worker.stop();
    }

    return 0;
}

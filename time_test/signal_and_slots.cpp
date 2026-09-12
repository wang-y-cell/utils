/**
 * signal_and_slots 微基准（time_test）
 *
 *   cmake --build build --target time_signal
 */

#include "Tools/runtime_test.h"
#include "concurrency/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using utils::bench::escape;
using utils::bench::print_title;
using utils::bench::time_us;
using utils::connection_type;
using utils::scoped_connection;
using utils::slots_t;
using utils::thread;

namespace {

constexpr int N = 200000;

class counter {
public:
    slots_t<> on_hit() {
        ++hits;
        return {};
    }

    slots_t<> on_hit_arg(int v) {
        hits += v;
        escape(&hits);
        return {};
    }

    void hit_direct() {
        ++hits;
        escape(&hits);
    }

    std::uint64_t hits = 0;
};

struct virt_base {
    virtual ~virt_base() = default;
    virtual void hit() = 0;
};

struct virt_counter : virt_base {
    void hit() override {
        ++hits;
        escape(&hits);
    }
    std::uint64_t hits = 0;
};

void free_hit(std::uint64_t *p) {
    ++*p;
    escape(p);
}

class emitter {
public:
    utils::signal<> ping;
    utils::signal<int> ping_int;
};

void print_ns_per(const char *name, long long us, int n) {
    const double ns = (n > 0) ? (us * 1000.0 / n) : 0.0;
    std::cout << "  " << name << ": " << us << " us (" << (us / 1000.0)
              << " ms)  ~" << ns << " ns/op\n";
}

} // namespace

int main() {
    {
        std::uint64_t sink = 0;
        const auto raw = time_us([&] {
            for (int i = 0; i < N; ++i) {
                ++sink;
                escape(&sink);
            }
        });

        counter plain;
        plain.hits = 0;
        const auto member = time_us([&] {
            for (int i = 0; i < N; ++i) {
                plain.hit_direct();
            }
        });

        virt_counter vc;
        virt_base *vb = &vc;
        const auto virt = time_us([&] {
            for (int i = 0; i < N; ++i) {
                vb->hit();
            }
        });

        void (*fp)(std::uint64_t *) = free_hit;
        const auto via_fp = time_us([&] {
            for (int i = 0; i < N; ++i) {
                fp(&sink);
            }
        });

        void (counter::*pmf)() = &counter::hit_direct;
        counter *pc = &plain;
        plain.hits = 0;
        const auto via_pmf = time_us([&] {
            for (int i = 0; i < N; ++i) {
                (pc->*pmf)();
            }
        });

        auto lambda = [&] {
            ++sink;
            escape(&sink);
        };
        const auto via_lambda = time_us([&] {
            for (int i = 0; i < N; ++i) {
                lambda();
            }
        });

        std::function<void()> fn = [&] {
            ++sink;
            escape(&sink);
        };
        const auto via_fn = time_us([&] {
            for (int i = 0; i < N; ++i) {
                fn();
            }
        });

        emitter src;
        auto dst = std::make_shared<counter>();
        scoped_connection c{utils::connect(src.ping, dst, &counter::on_hit,
                                           connection_type::direct)};
        dst->hits = 0;
        const auto direct1 = time_us([&] {
            for (int i = 0; i < N; ++i) {
                src.ping.emit();
            }
        });

        print_title("same-thread Direct  N=200000  (1 slot vs call kinds)");
        print_ns_per("raw ++", raw, N);
        print_ns_per("member call", member, N);
        print_ns_per("virtual call", virt, N);
        print_ns_per("function pointer", via_fp, N);
        print_ns_per("member pointer", via_pmf, N);
        print_ns_per("lambda (direct)", via_lambda, N);
        print_ns_per("std::function", via_fn, N);
        print_ns_per("signal Direct 1 slot", direct1, N);
        std::cout << "  (signal hits=" << dst->hits << ", virt hits=" << vc.hits
                  << ", member hits=" << plain.hits << ")\n\n";
    }

    {
        emitter src8;
        std::shared_ptr<counter> slots[8];
        std::vector<scoped_connection> cons;
        for (auto &d : slots) {
            d = std::make_shared<counter>();
            cons.emplace_back(utils::connect(src8.ping, d, &counter::on_hit,
                                             connection_type::direct));
        }
        const auto eight = time_us([&] {
            for (int i = 0; i < N; ++i) {
                src8.ping.emit();
            }
        });

        emitter src1;
        auto one = std::make_shared<counter>();
        scoped_connection c1{utils::connect(src1.ping, one, &counter::on_hit,
                                            connection_type::direct)};
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
        std::cout << "\n";
    }

    {
        emitter src;
        auto dst = std::make_shared<counter>();
        scoped_connection c{utils::connect(src.ping_int, dst, &counter::on_hit_arg,
                                           connection_type::direct)};
        dst->hits = 0;
        const auto with_arg = time_us([&] {
            for (int i = 0; i < N; ++i) {
                src.ping_int.emit(1);
            }
        });
        print_title("same-thread Direct  N=200000  (signal<int>)");
        print_ns_per("Direct 1 slot + int", with_arg, N);
        std::cout << "  (hits=" << dst->hits << ")\n\n";
    }

    {
        constexpr int N_x = 20000;
        thread worker;
        worker.start();

        emitter src;
        auto dst = std::make_shared<counter>();
        scoped_connection c{utils::connect(src.ping, dst, &counter::on_hit,
                                           &worker, connection_type::queued)};

        for (int i = 0; i < 200 &&
                        !(worker.loop() && worker.loop()->is_pumping());
             ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        dst->hits = 0;
        const auto cross = time_us([&] {
            for (int i = 0; i < N_x; ++i) {
                src.ping.emit();
            }
            while (dst->hits < static_cast<std::uint64_t>(N_x)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

        print_title(
            "cross-thread Queued  N=20000  (emit + wait worker drain)");
        print_ns_per("Queued cross-thread", cross, N_x);
        std::cout << "  (hits=" << dst->hits << ")\n";

        worker.stop();
    }

    return 0;
}

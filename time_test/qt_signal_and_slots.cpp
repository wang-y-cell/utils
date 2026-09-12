/**
 * Qt 6 信号槽微基准（与 time_test/signal_and_slots.cpp 场景对齐）
 *
 *   cmake -S . -B build_qt -DCMAKE_BUILD_TYPE=Release ^
 *     -DCMAKE_PREFIX_PATH=F:/Qt/6.8.3/mingw_64 ^
 *     -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw1310_64/bin/g++.exe ^
 *     -DCMAKE_C_COMPILER=F:/Qt/Tools/mingw1310_64/bin/gcc.exe
 *   cmake --build build_qt --target time_qt_signal time_signal
 */

#include "Tools/runtime_test.h"
#include "qt_signal_bench.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include <QtGlobal>

#include <cstdint>
#include <functional>
#include <iostream>
#include <vector>

using utils::bench::escape;
using utils::bench::print_title;
using utils::bench::time_us;

namespace {

constexpr int N = 200000;

struct plain_counter {
    std::uint64_t hits = 0;
    void hit_direct() {
        ++hits;
        escape(&hits);
    }
};

struct virt_base {
    virtual ~virt_base() = default;
    virtual void hit() = 0;
};

struct virt_counter : virt_base {
    std::uint64_t hits = 0;
    void hit() override {
        ++hits;
        escape(&hits);
    }
};

void free_hit(std::uint64_t* p) {
    ++*p;
    escape(p);
}

void print_ns_per(const char* name, long long us, int n) {
    const double ns = (n > 0) ? (us * 1000.0 / n) : 0.0;
    std::cout << "  " << name << ": " << us << " us (" << (us / 1000.0)
              << " ms)  ~" << ns << " ns/op\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // ---------- 1) 同线程 Direct：相对各类函数调用 ----------
    {
        std::uint64_t sink = 0;
        const auto raw = time_us([&] {
            for (int i = 0; i < N; ++i) {
                ++sink;
                escape(&sink);
            }
        });

        plain_counter plain;
        plain.hits = 0;
        const auto member = time_us([&] {
            for (int i = 0; i < N; ++i) {
                plain.hit_direct();
            }
        });

        virt_counter vc;
        virt_base* vb = &vc;
        const auto virt = time_us([&] {
            for (int i = 0; i < N; ++i) {
                vb->hit();
            }
        });

        void (*fp)(std::uint64_t*) = free_hit;
        const auto via_fp = time_us([&] {
            for (int i = 0; i < N; ++i) {
                fp(&sink);
            }
        });

        void (plain_counter::*pmf)() = &plain_counter::hit_direct;
        plain_counter* pc = &plain;
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
        const auto stdfn = time_us([&] {
            for (int i = 0; i < N; ++i) {
                fn();
            }
        });

        QtEmitter src;
        QtCounter dst;
        QObject::connect(&src, &QtEmitter::ping, &dst, &QtCounter::on_hit,
                         Qt::DirectConnection);
        dst.hits = 0;
        const auto direct1 = time_us([&] {
            for (int i = 0; i < N; ++i) {
                emit src.ping();
            }
        });
        escape(&dst.hits);

        print_title(
            "[Qt] same-thread Direct  N=200000  (1 slot vs call kinds)");
        print_ns_per("raw ++", raw, N);
        print_ns_per("member call", member, N);
        print_ns_per("virtual call", virt, N);
        print_ns_per("function pointer", via_fp, N);
        print_ns_per("member pointer", via_pmf, N);
        print_ns_per("lambda (direct)", via_lambda, N);
        print_ns_per("std::function", stdfn, N);
        print_ns_per("Qt Direct 1 slot", direct1, N);
        std::cout << "  (Qt hits=" << dst.hits << ", virt hits=" << vc.hits
                  << ", member hits=" << plain.hits << ")\n\n";
    }

    // ---------- 2) Direct：0 / 1 / 8 槽 ----------
    {
        QtEmitter src8;
        QtCounter receivers[8];
        for (auto& d : receivers) {
            QObject::connect(&src8, &QtEmitter::ping, &d, &QtCounter::on_hit,
                             Qt::DirectConnection);
            d.hits = 0;
        }
        const auto eight = time_us([&] {
            for (int i = 0; i < N; ++i) {
                emit src8.ping();
            }
        });

        QtEmitter src1;
        QtCounter one;
        QObject::connect(&src1, &QtEmitter::ping, &one, &QtCounter::on_hit,
                         Qt::DirectConnection);
        one.hits = 0;
        const auto one_t = time_us([&] {
            for (int i = 0; i < N; ++i) {
                emit src1.ping();
            }
        });

        QtEmitter bare;
        const auto zero = time_us([&] {
            for (int i = 0; i < N; ++i) {
                emit bare.ping();
            }
        });

        print_title("[Qt] same-thread Direct  N=200000  (0 / 1 / 8 slots)");
        print_ns_per("0 slots", zero, N);
        print_ns_per("1 slot", one_t, N);
        print_ns_per("8 slots", eight, N);
        std::cout << '\n';
    }

    // ---------- 3) 同线程 Queued：emit 全部入队再排空 ----------
    {
        QtEmitter src;
        QtCounter dst;
        QObject::connect(&src, &QtEmitter::ping, &dst, &QtCounter::on_hit,
                         Qt::QueuedConnection);
        dst.hits = 0;
        const auto queued = time_us([&] {
            for (int i = 0; i < N; ++i) {
                emit src.ping();
            }
            QCoreApplication::processEvents();
        });

        print_title(
            "[Qt] same-thread Queued  N=200000  (emit all then processEvents)");
        print_ns_per("Queued emit+drain", queued, N);
        std::cout << "  (hits=" << dst.hits << ")\n\n";
    }

    // ---------- 4) 带 int 参数 Direct ----------
    {
        QtEmitter src;
        QtCounter dst;
        QObject::connect(&src, &QtEmitter::ping_int, &dst,
                         &QtCounter::on_hit_arg, Qt::DirectConnection);
        dst.hits = 0;
        const auto with_arg = time_us([&] {
            for (int i = 0; i < N; ++i) {
                emit src.ping_int(1);
            }
        });
        print_title("[Qt] same-thread Direct  N=200000  (signal int)");
        print_ns_per("Direct 1 slot + int", with_arg, N);
        std::cout << "  (hits=" << dst.hits << ")\n\n";
    }

    // ---------- 5) 跨线程 Queued ----------
    {
        constexpr int N_x = 20000;
        QThread worker;
        QtEmitter src;
        QtCounter dst;
        dst.moveToThread(&worker);
        QObject::connect(&src, &QtEmitter::ping, &dst, &QtCounter::on_hit,
                         Qt::QueuedConnection);
        worker.start();

        dst.hits = 0;
        const auto cross = time_us([&] {
            for (int i = 0; i < N_x; ++i) {
                emit src.ping();
            }
            while (dst.hits < static_cast<std::uint64_t>(N_x)) {
                QThread::usleep(50);
            }
        });

        print_title(
            "[Qt] cross-thread Queued  N=20000  (emit + wait worker drain)");
        print_ns_per("Queued cross-thread", cross, N_x);
        std::cout << "  (hits=" << dst.hits << ")\n";

        worker.quit();
        worker.wait();
    }

    return 0;
}

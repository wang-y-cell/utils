/**
 * memory 微基准（原 main.cpp）
 *
 *   cmake --build build --target time_memory
 *   ./build/time_memory
 *
 * 分层：
 *   -DUTILS_POOL_LEAK_CHECK=0（默认）→ 只比速度
 *   -DUTILS_POOL_LEAK_CHECK=1       → 只报 memory_allocator 浪费分项
 */
#ifndef UTILS_POOL_LEAK_CHECK
#define UTILS_POOL_LEAK_CHECK 0
#endif

#include "Tools/runtime_test.h"
#include "demo/memory/sgi_alloc.h"
#include "memory/memory_allocator.h"
#include "memory/memory_pool.h"
#include "memory/typed_alloc.h"

#include <cstddef>
#include <iostream>
#include <random>
#include <string_view>
#include <vector>

namespace {

#if UTILS_POOL_LEAK_CHECK >= 1
void print_waste(std::string_view name, const utils::memory_allocator& alloc) {
    using utils::bench::bytes_human;
    std::cout << "  " << name << ": rounding=" << alloc.rounding_waste()
              << " (" << bytes_human(alloc.rounding_waste()) << ")"
              << "  class_table=" << alloc.class_table_bytes()
              << " (" << bytes_human(alloc.class_table_bytes()) << ")"
              << "  (classes=" << alloc.size_class_count() << ")\n";
}
#endif

}  // namespace

int main() {
    using utils::bench::escape;
    using utils::bench::print_row;
    using utils::bench::print_title;
    using utils::bench::time_us;

    constexpr int N = 200000;

#if UTILS_POOL_LEAK_CHECK == 0
    // ---------- 档 0：计时 ----------
    {
        const auto sgi = time_us([&] {
            for (int i = 0; i < N; ++i) {
                void* p = msl::alloc::allocate(64);
                escape(p);
                msl::alloc::deallocate(p, 64);
            }
        });
        const auto pool = time_us([&] {
            utils::memory_pool mp(64);
            for (int i = 0; i < N; ++i) {
                void* p = mp.allocate();
                escape(p);
                mp.deallocate_unchecked(p);
            }
        });
        const auto facade = time_us([&] {
            utils::memory_allocator alloc;
            for (int i = 0; i < N; ++i) {
                void* p = alloc.allocate(64);
                escape(p);
                alloc.deallocate_unchecked(p, 64);
            }
        });
        const auto typed = time_us([&] {
            for (int i = 0; i < N; ++i) {
                char* p = utils::typed_alloc<char>::allocate(64);
                escape(p);
                utils::typed_alloc<char>::deallocate(p, 64);
            }
        });
        const auto neu = time_us([&] {
            for (int i = 0; i < N; ++i) {
                void* p = ::operator new(64);
                escape(p);
                ::operator delete(p);
            }
        });

        print_title("fixed 64B, alloc+free immediately  N=200000  "
                    "(LEAK_CHECK=0 timing)");
        print_row("sgi default_alloc", sgi);
        print_row("utils::memory_pool", pool);
        print_row("utils::memory_allocator", facade);
        print_row("utils::typed_alloc<char>", typed);
        print_row("::operator new/delete", neu);
        std::cout << '\n';
    }

    {
        std::vector<std::size_t> sizes(static_cast<std::size_t>(N));
        std::mt19937 rng(42);
        std::uniform_int_distribution<std::size_t> dist(1, 128);
        for (auto& s : sizes) s = dist(rng);

        const auto sgi = time_us([&] {
            for (std::size_t s : sizes) {
                void* p = msl::alloc::allocate(s);
                escape(p);
                msl::alloc::deallocate(p, s);
            }
        });
        const auto facade = time_us([&] {
            utils::memory_allocator alloc;
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
        });
        const auto neu = time_us([&] {
            for (std::size_t s : sizes) {
                void* p = ::operator new(s);
                escape(p);
                ::operator delete(p);
            }
        });

        print_title("random [1,128], alloc+free immediately  N=200000  "
                    "(LEAK_CHECK=0 timing)");
        print_row("sgi default_alloc", sgi);
        print_row("utils::memory_allocator", facade);
        print_row("::operator new/delete", neu);
        std::cout << '\n';
    }

    {
        std::vector<std::size_t> sizes(static_cast<std::size_t>(N));
        std::mt19937 rng(43);
        std::uniform_int_distribution<std::size_t> dist(1, 4096);
        for (auto& s : sizes) s = dist(rng);

        const auto sgi = time_us([&] {
            for (std::size_t s : sizes) {
                void* p = msl::alloc::allocate(s);
                escape(p);
                msl::alloc::deallocate(p, s);
            }
        });
        const auto facade = time_us([&] {
            utils::memory_allocator alloc;
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
        });
        const auto facade_3band = time_us([&] {
            utils::memory_allocator alloc(utils::size_class_config::from_bands(
                {{8, 128}, {64, 1024}, {256, 4096}}));
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
        });
        const auto facade_1band = time_us([&] {
            auto cfg = utils::size_class_config::from_bands({{8, 4096}});
            cfg.max_classes = 0;
            utils::memory_allocator alloc(cfg);
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
        });
        const auto neu = time_us([&] {
            for (std::size_t s : sizes) {
                void* p = ::operator new(s);
                escape(p);
                ::operator delete(p);
            }
        });

        print_title("random [1,4096], alloc+free immediately  N=200000  "
                    "(LEAK_CHECK=0 timing)");
        print_row("sgi default_alloc", sgi);
        print_row("memory_allocator (2-band default)", facade);
        print_row("memory_allocator (3-band)", facade_3band);
        print_row("memory_allocator (1-band 8B unlimited)", facade_1band);
        print_row("::operator new/delete", neu);
        std::cout << '\n';
    }

    {
        constexpr int N_mb = 3000;
        constexpr std::size_t MB = 1024u * 1024u;

        std::vector<std::size_t> sizes(static_cast<std::size_t>(N_mb));
        std::mt19937 rng(44);
        std::uniform_int_distribution<std::size_t> dist(1 * MB, 5 * MB);
        for (auto& s : sizes) s = dist(rng);

        auto mb_cfg = utils::size_class_config::from_bands({{MB, 5 * MB}});
        mb_cfg.large_threshold = 5 * MB;
        mb_cfg.refill_objects = 2;

        const auto facade_mb = time_us([&] {
            utils::memory_allocator alloc(mb_cfg);
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
        });
        const auto buckets = time_us([&] {
            utils::memory_pool p1(1 * MB, 2);
            utils::memory_pool p2(2 * MB, 2);
            utils::memory_pool p3(3 * MB, 2);
            utils::memory_pool p4(4 * MB, 2);
            utils::memory_pool p5(5 * MB, 2);
            utils::memory_pool* pools[5] = {&p1, &p2, &p3, &p4, &p5};
            for (std::size_t s : sizes) {
                std::size_t idx = (s + MB - 1) / MB;
                if (idx < 1) idx = 1;
                if (idx > 5) idx = 5;
                utils::memory_pool& pool = *pools[idx - 1];
                void* p = pool.allocate();
                escape(p);
                pool.deallocate_unchecked(p);
            }
        });
        const auto fixed5 = time_us([&] {
            utils::memory_pool pool(5 * MB, 2);
            for (std::size_t s : sizes) {
                (void)s;
                void* p = pool.allocate();
                escape(p);
                pool.deallocate_unchecked(p);
            }
        });
        const auto facade_default = time_us([&] {
            utils::memory_allocator alloc;
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
        });
        const auto neu = time_us([&] {
            for (std::size_t s : sizes) {
                void* p = ::operator new(s);
                escape(p);
                ::operator delete(p);
            }
        });

        print_title("random [1MiB,5MiB], alloc+free immediately  N=3000  "
                    "(LEAK_CHECK=0 timing)");
        print_row("memory_allocator (1-band 1..5MiB)", facade_mb);
        print_row("memory_pool x5 buckets", buckets);
        print_row("memory_pool fixed 5MiB", fixed5);
        print_row("memory_allocator (default ~4KiB)", facade_default);
        print_row("::operator new/delete", neu);
    }

#elif UTILS_POOL_LEAK_CHECK >= 1
    // ---------- 档 1+：只报浪费（stdout，与计时分离）----------
    print_title("memory_allocator waste  (LEAK_CHECK>=1; rounding vs "
                "class_table separate)");

    {
        utils::memory_allocator alloc;
        for (int i = 0; i < N; ++i) {
            void* p = alloc.allocate(64);
            escape(p);
            alloc.deallocate_unchecked(p, 64);
        }
        print_waste("fixed 64B / default", alloc);
    }

    {
        std::vector<std::size_t> sizes(static_cast<std::size_t>(N));
        std::mt19937 rng(42);
        std::uniform_int_distribution<std::size_t> dist(1, 128);
        for (auto& s : sizes) s = dist(rng);

        utils::memory_allocator alloc;
        for (std::size_t s : sizes) {
            void* p = alloc.allocate(s);
            escape(p);
            alloc.deallocate(p, s);
        }
        print_waste("random [1,128] / default", alloc);
    }

    {
        std::vector<std::size_t> sizes(static_cast<std::size_t>(N));
        std::mt19937 rng(43);
        std::uniform_int_distribution<std::size_t> dist(1, 4096);
        for (auto& s : sizes) s = dist(rng);

        {
            utils::memory_allocator alloc;
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
            print_waste("random [1,4096] / 2-band default", alloc);
        }
        {
            utils::memory_allocator alloc(utils::size_class_config::from_bands(
                {{8, 128}, {64, 1024}, {256, 4096}}));
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
            print_waste("random [1,4096] / 3-band", alloc);
        }
        {
            auto cfg = utils::size_class_config::from_bands({{8, 4096}});
            cfg.max_classes = 0;
            utils::memory_allocator alloc(cfg);
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
            print_waste("random [1,4096] / 1-band 8B unlimited", alloc);
        }
    }

    {
        constexpr int N_mb = 3000;
        constexpr std::size_t MB = 1024u * 1024u;
        std::vector<std::size_t> sizes(static_cast<std::size_t>(N_mb));
        std::mt19937 rng(44);
        std::uniform_int_distribution<std::size_t> dist(1 * MB, 5 * MB);
        for (auto& s : sizes) s = dist(rng);

        auto mb_cfg = utils::size_class_config::from_bands({{MB, 5 * MB}});
        mb_cfg.large_threshold = 5 * MB;
        mb_cfg.refill_objects = 2;

        {
            utils::memory_allocator alloc(mb_cfg);
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
            print_waste("random [1MiB,5MiB] / 1-band 1..5MiB", alloc);
        }
        {
            utils::memory_allocator alloc;
            for (std::size_t s : sizes) {
                void* p = alloc.allocate(s);
                escape(p);
                alloc.deallocate(p, s);
            }
            print_waste("random [1MiB,5MiB] / default ~4KiB", alloc);
        }
    }

    std::cout << '\n';
#else
#error "UTILS_POOL_LEAK_CHECK must be 0 (timing) or >=1 (waste)"
#endif

    return 0;
}

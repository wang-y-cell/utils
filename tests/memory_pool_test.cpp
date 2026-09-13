#include "memory/memory_pool.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// =============================================================================
// memory_pool — 构造 / 查询
// =============================================================================

TEST(MemoryPool, RejectsInvalidConstructorArgs) {
    EXPECT_THROW(utils::memory_pool(0), std::invalid_argument);
    EXPECT_THROW(utils::memory_pool(16, 0), std::invalid_argument);
    EXPECT_THROW(utils::memory_pool(16, 8, 3), std::invalid_argument);
    EXPECT_THROW(utils::memory_pool(16, 8, 0), std::invalid_argument);
}

TEST(MemoryPool, BlockSizeRoundedUpForFreeNodeAndAlignment) {
    utils::memory_pool tiny(1, 4, 8);
    // 至少 sizeof(free_node*)，且按 alignment 上取整
    EXPECT_GE(tiny.block_size(), sizeof(void*));
    EXPECT_EQ(tiny.block_size() % 8, 0u);
    EXPECT_EQ(tiny.alignment(), 8u);

    utils::memory_pool aligned(24, 2, 32);
    EXPECT_EQ(aligned.block_size() % 32, 0u);
    EXPECT_GE(aligned.block_size(), 24u);
    EXPECT_EQ(aligned.alignment(), 32u);
}

TEST(MemoryPool, DefaultAlignmentAtLeastMaxAlign) {
    utils::memory_pool pool(64);
    EXPECT_GE(pool.alignment(), alignof(std::max_align_t));
    EXPECT_EQ(pool.capacity(), 0u);
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.in_use(), 0u);
}

// =============================================================================
// memory_pool — allocate / deallocate
// =============================================================================

TEST(MemoryPool, AllocateGrowsCapacityAndReportsStats) {
    utils::memory_pool pool(24, 2, 32);
    EXPECT_EQ(pool.capacity(), 0u);

    void* a = pool.allocate();
    void* b = pool.allocate();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(a) % 32, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b) % 32, 0u);
    EXPECT_EQ(pool.capacity(), 2u);
    EXPECT_EQ(pool.in_use(), 2u);
    EXPECT_EQ(pool.available(), 0u);

    void* c = pool.allocate(); // 触发扩容（next_size 加倍 → +4，总量 2+4）
    EXPECT_EQ(pool.capacity(), 6u);
    EXPECT_EQ(pool.in_use(), 3u);
    EXPECT_EQ(pool.available(), 3u);

    pool.deallocate(a);
    pool.deallocate(b);
    pool.deallocate(c);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.available(), pool.capacity());
}

TEST(MemoryPool, DeallocateNullptrIsNoop) {
    utils::memory_pool pool(32, 4);
    pool.deallocate(nullptr);
    EXPECT_EQ(pool.in_use(), 0u);

    void* p = pool.allocate();
    pool.deallocate(nullptr);
    EXPECT_EQ(pool.in_use(), 1u);
    pool.deallocate(p);
}

TEST(MemoryPool, FreelistReuseSamePointer) {
    utils::memory_pool pool(24, 2, 32);
    void* a = pool.allocate();
    void* b = pool.allocate();
    pool.deallocate(a);
    void* reused = pool.allocate();
    EXPECT_EQ(reused, a);

    pool.deallocate(reused);
    pool.deallocate(b);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(MemoryPool, DeallocateUncheckedReuses) {
    utils::memory_pool pool(32, 4);
    void* a = pool.allocate();
    pool.deallocate_unchecked(a);
    void* b = pool.allocate();
    EXPECT_EQ(a, b);
    pool.deallocate_unchecked(b);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(MemoryPool, ManyAllocationsGrowWithDoubling) {
    utils::memory_pool pool(16, 2);
    std::vector<void*> ptrs;
    ptrs.reserve(20);
    for (int i = 0; i < 20; ++i) {
        ptrs.push_back(pool.allocate());
    }
    EXPECT_EQ(pool.in_use(), 20u);
    EXPECT_GE(pool.capacity(), 20u);
    // 2 + 4 + 8 + 16 = 30
    EXPECT_EQ(pool.capacity(), 30u);

    for (void* p : ptrs) {
        pool.deallocate(p);
    }
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.available(), pool.capacity());
}

TEST(MemoryPool, AllocateThenFullReturnRestoresAvailable) {
    utils::memory_pool pool(48, 8, 16);
    constexpr int N = 8;
    void* ptrs[N];
    for (int i = 0; i < N; ++i) {
        ptrs[i] = pool.allocate();
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptrs[i]) % 16, 0u);
    }
    EXPECT_EQ(pool.capacity(), 8u);
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.in_use(), 8u);

    for (int i = 0; i < N; ++i) {
        pool.deallocate(ptrs[i]);
    }
    EXPECT_EQ(pool.available(), 8u);
    EXPECT_EQ(pool.in_use(), 0u);
}

#if UTILS_POOL_LEAK_CHECK >= 2

TEST(MemoryPool, DumpLeaksShowsAllocationSite) {
    utils::memory_pool pool(32, 4);
    void* p = pool.allocate();
    EXPECT_EQ(pool.in_use(), 1u);

    testing::internal::CaptureStderr();
    pool.dump_leaks();
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("outstanding=1"), std::string::npos);
    EXPECT_NE(log.find("MemoryPool_DumpLeaksShowsAllocationSite"),
              std::string::npos);

    pool.deallocate(p);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(MemoryPool, DeallocateClearsLeakSite) {
    utils::memory_pool pool(16, 2);
    void* a = pool.allocate();
    void* b = pool.allocate();
    pool.deallocate(a);
    EXPECT_EQ(pool.in_use(), 1u);

    testing::internal::CaptureStderr();
    pool.dump_leaks();
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("outstanding=1"), std::string::npos);

    pool.deallocate(b);
    EXPECT_EQ(pool.in_use(), 0u);
}

#endif  // UTILS_POOL_LEAK_CHECK >= 2

// =============================================================================
// memory_pool — 边界 / 扩容上限
// =============================================================================

TEST(MemoryPool, AlignmentRaisedToPointerAlignment) {
    // 请求 alignment=1（合法 2 的幂），实现会抬到至少 alignof(void*)
    utils::memory_pool pool(16, 4, 1);
    EXPECT_GE(pool.alignment(), alignof(void*));
    void* p = pool.allocate();
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % pool.alignment(), 0u);
    pool.deallocate(p);
}

TEST(MemoryPool, GrowChunkSizeCapsAtAbout1MiB) {
    // block_size=64KiB → 上限 next_size ≈ 1MiB/64KiB = 16
    constexpr std::size_t kBlock = 64u * 1024u;
    utils::memory_pool pool(kBlock, 4, alignof(std::max_align_t));

    std::vector<void*> ptrs;
    // 4 + 8 + 16 + 16 + 16 ... 吃掉几次「封顶后」的扩容
    const std::size_t need = 4 + 8 + 16 + 16 + 16;
    ptrs.reserve(need);
    for (std::size_t i = 0; i < need; ++i) {
        ptrs.push_back(pool.allocate());
    }
    // 4+8+16+16+16 = 60
    EXPECT_EQ(pool.capacity(), 60u);
    EXPECT_EQ(pool.in_use(), need);

    for (void* p : ptrs) {
        pool.deallocate(p);
    }
    EXPECT_EQ(pool.in_use(), 0u);
}

#if UTILS_POOL_LEAK_CHECK >= 2

TEST(MemoryPool, LeakFileEnvWritesOnDestroy) {
    const char* path = "utils_pool_leak_test_out.txt";
    std::remove(path);
#ifdef _WIN32
    ASSERT_EQ(_putenv_s("UTILS_POOL_LEAK_FILE", path), 0);
#else
    ASSERT_EQ(setenv("UTILS_POOL_LEAK_FILE", path, 1), 0);
#endif
    {
        testing::internal::CaptureStderr();
        {
            utils::memory_pool pool(32, 2);
            (void)pool.allocate(); // 故意泄漏，析构写文件
        }
        (void)testing::internal::GetCapturedStderr();
    }
#ifdef _WIN32
    _putenv_s("UTILS_POOL_LEAK_FILE", "");
#else
    unsetenv("UTILS_POOL_LEAK_FILE");
#endif

    FILE* f = std::fopen(path, "r");
    ASSERT_NE(f, nullptr);
    char buf[512]{};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::remove(path);
    ASSERT_GT(n, 0u);
    const std::string log(buf, n);
    EXPECT_NE(log.find("outstanding="), std::string::npos);
}

#endif  // UTILS_POOL_LEAK_CHECK >= 2

// =============================================================================
// memory_pool — 压力
// =============================================================================

TEST(MemoryPoolStress, AllocateFreeChurn) {
    constexpr int rounds = 200;
    constexpr int batch = 1024;
    utils::memory_pool pool(64, 32);
    std::vector<void*> ptrs;
    ptrs.reserve(batch);

    for (int r = 0; r < rounds; ++r) {
        ptrs.clear();
        for (int i = 0; i < batch; ++i) {
            ptrs.push_back(pool.allocate());
        }
        EXPECT_EQ(pool.in_use(), static_cast<std::size_t>(batch));
        // 交错归还，打乱 freelist 顺序
        for (int i = 0; i < batch; i += 2) {
            pool.deallocate(ptrs[static_cast<std::size_t>(i)]);
        }
        for (int i = 1; i < batch; i += 2) {
            pool.deallocate(ptrs[static_cast<std::size_t>(i)]);
        }
        EXPECT_EQ(pool.in_use(), 0u);
        EXPECT_EQ(pool.available(), pool.capacity());
    }
}

TEST(MemoryPoolStress, ReuseStability) {
    utils::memory_pool pool(48, 16);
    void* first = pool.allocate();
    pool.deallocate(first);
    for (int i = 0; i < 50000; ++i) {
        void* p = pool.allocate();
        EXPECT_EQ(p, first);
        pool.deallocate_unchecked(p);
    }
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(MemoryPoolStress, ConcurrentWithExternalMutex) {
    utils::memory_pool pool(32, 64);
    std::mutex mu;
    constexpr int threads_n = 4;
    constexpr int ops = 5000;
    std::vector<std::thread> workers;
    workers.reserve(threads_n);

    for (int t = 0; t < threads_n; ++t) {
        workers.emplace_back([&] {
            std::vector<void*> local;
            local.reserve(64);
            for (int i = 0; i < ops; ++i) {
                {
                    std::lock_guard<std::mutex> lock(mu);
                    local.push_back(pool.allocate());
                }
                if (local.size() >= 64) {
                    for (void* p : local) {
                        std::lock_guard<std::mutex> lock(mu);
                        pool.deallocate(p);
                    }
                    local.clear();
                }
            }
            for (void* p : local) {
                std::lock_guard<std::mutex> lock(mu);
                pool.deallocate(p);
            }
        });
    }
    for (auto& th : workers) {
        th.join();
    }
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.available(), pool.capacity());
}

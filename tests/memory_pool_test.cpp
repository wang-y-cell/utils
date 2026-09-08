#include "memory/memory_pool.h"

#include <cstdint>
#include <stdexcept>
#include <string>
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

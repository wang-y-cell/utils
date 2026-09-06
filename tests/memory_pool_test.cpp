#include "memory/memory_pool.h"

#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

TEST(MemoryPool, RejectsInvalidConstructorArgs) {
    EXPECT_THROW(utils::memory_pool(0), std::invalid_argument);
    EXPECT_THROW(utils::memory_pool(16, 0), std::invalid_argument);
    EXPECT_THROW(utils::memory_pool(16, 8, 3), std::invalid_argument);
}

TEST(MemoryPool, GrowsAndReusesBlocks) {
    utils::memory_pool pool(24, 2, 32);
    EXPECT_EQ(pool.capacity(), 0u);

    void* a = pool.allocate();
    void* b = pool.allocate();
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(a) % 32, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b) % 32, 0u);
    EXPECT_EQ(pool.capacity(), 2u);
    EXPECT_EQ(pool.in_use(), 2u);

    void* c = pool.allocate();
    EXPECT_EQ(pool.capacity(), 4u);
    EXPECT_EQ(pool.in_use(), 3u);

    pool.deallocate(a);
    void* reused = pool.allocate();
    EXPECT_EQ(reused, a);

    pool.deallocate(reused);
    pool.deallocate(b);
    pool.deallocate(c);
    pool.deallocate(nullptr);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.available(), pool.capacity());
}

TEST(MemoryPool, UncheckedDeallocateReuses) {
    utils::memory_pool pool(32, 4);
    void* a = pool.allocate();
    pool.deallocate_unchecked(a);
    void* b = pool.allocate();
    EXPECT_EQ(a, b);
    pool.deallocate_unchecked(b);
}

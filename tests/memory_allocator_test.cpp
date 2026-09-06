#include "memory/memory_allocator.h"
#include "memory/memory_pool.h"
#include "memory/memory_resource.h"
#include "memory/object_pool.h"
#include "memory/typed_alloc.h"

#include <string>

#include <gtest/gtest.h>

#if UTILS_POOL_LEAK_CHECK >= 2

TEST(MemoryPoolLeak, DumpShowsAllocationSite) {
    utils::memory_pool pool(32, 4);
    void* p = pool.allocate();
    EXPECT_EQ(pool.in_use(), 1u);

    testing::internal::CaptureStderr();
    pool.dump_leaks();
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("outstanding=1"), std::string::npos);
    EXPECT_NE(log.find("MemoryPoolLeak_DumpShowsAllocationSite"),
              std::string::npos);

    pool.deallocate(p);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(MemoryPoolLeak, DeallocateClearsSite) {
    utils::memory_pool pool(16, 2);
    void* a = pool.allocate();
    void* b = pool.allocate();
    pool.deallocate(a);
    EXPECT_EQ(pool.in_use(), 1u);
    pool.deallocate(b);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ObjectPoolLeak, CreateTracksViaStorage) {
    utils::object_pool<int> objects(4);
    int* p = objects.create(42);
    EXPECT_EQ(objects.in_use(), 1u);
    EXPECT_EQ(*p, 42);
    objects.destroy(p);
    EXPECT_EQ(objects.in_use(), 0u);
}

TEST(FixedBlockResource, RoutesThroughPool) {
    utils::memory_pool pool(64, 2);
    utils::fixed_block_resource<utils::memory_pool> res(pool, "test_fixed");
    EXPECT_EQ(res.kind(), utils::pool_kind::fixed);
    void* p = res.allocate(32);
    EXPECT_EQ(pool.in_use(), 1u);
    res.deallocate(p, 32);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_THROW((void)res.allocate(128), std::invalid_argument);
}

TEST(MemoryAllocatorLeak, TracksOutstandingAndSite) {
    utils::memory_allocator alloc;
    void* p = alloc.allocate(64);
    EXPECT_EQ(alloc.outstanding(), 1u);
    testing::internal::CaptureStderr();
    alloc.dump_leaks();
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("outstanding=1"), std::string::npos);
    EXPECT_NE(log.find("MemoryAllocatorLeak_TracksOutstandingAndSite"),
              std::string::npos);
    alloc.deallocate(p, 64);
    EXPECT_EQ(alloc.outstanding(), 0u);
}

#endif  // UTILS_POOL_LEAK_CHECK >= 2

TEST(MemoryAllocator, SizeClassAndRawNew) {
    utils::memory_allocator alloc;
    void* small = alloc.allocate(24);
    void* big = alloc.allocate(alloc.large_threshold() + 1);
#if UTILS_POOL_LEAK_CHECK >= 1
    EXPECT_EQ(alloc.outstanding(), 2u);
    EXPECT_EQ(alloc.rounding_waste(), 0u);  // 24 正好一档；大块走 new
    EXPECT_GT(alloc.class_table_bytes(), 0u);
#endif
    alloc.deallocate(small, 24);
    alloc.deallocate(big, alloc.large_threshold() + 1);
#if UTILS_POOL_LEAK_CHECK >= 1
    EXPECT_EQ(alloc.outstanding(), 0u);
#endif
}

#if UTILS_POOL_LEAK_CHECK >= 1
TEST(MemoryAllocator, WasteRoundingAndClassTableSeparate) {
    utils::memory_allocator alloc;
    const std::size_t table0 = alloc.class_table_bytes();
    EXPECT_GT(table0, 0u);
    EXPECT_EQ(alloc.rounding_waste(), 0u);

    // 7 → 8：rounding +1；25 → 32：+7
    void* a = alloc.allocate(7);
    void* b = alloc.allocate(25);
    EXPECT_EQ(alloc.rounding_waste(), 1u + 7u);
    // 档表开销固定，不随分配累加
    EXPECT_EQ(alloc.class_table_bytes(), table0);
    alloc.deallocate(a, 7);
    alloc.deallocate(b, 25);
    // lifetime rounding 不因归还回退
    EXPECT_EQ(alloc.rounding_waste(), 8u);

    auto unlimited = utils::size_class_config::from_bands({{8, 4096}});
    unlimited.max_classes = 0;
    utils::memory_allocator dense(unlimited);
    EXPECT_GT(dense.class_table_bytes(), table0);
    EXPECT_EQ(dense.size_class_count(), 512u);

    testing::internal::CaptureStderr();
    alloc.dump_waste();
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("rounding=8"), std::string::npos);
    EXPECT_NE(log.find("class_table="), std::string::npos);
}
#endif  // UTILS_POOL_LEAK_CHECK >= 1


TEST(MemoryAllocator, TwoLevelSizeClasses) {
    utils::memory_allocator alloc;
    EXPECT_EQ(alloc.band_count(), 2u);
    EXPECT_EQ(alloc.size_class_count(), 47u);
    EXPECT_EQ(alloc.round_up_size(1), 8u);
    EXPECT_EQ(alloc.round_up_size(7), 8u);
    EXPECT_EQ(alloc.round_up_size(8), 8u);
    EXPECT_EQ(alloc.round_up_size(9), 16u);
    EXPECT_EQ(alloc.round_up_size(128), 128u);
    EXPECT_EQ(alloc.round_up_size(129), 256u);
    EXPECT_EQ(alloc.round_up_size(2049), 2176u);
    EXPECT_EQ(alloc.round_up_size(4096), 4096u);
    EXPECT_EQ(alloc.round_up_size(4097), 0u);

    void* a = alloc.allocate(7);
    void* b = alloc.allocate(200);
    void* c = alloc.allocate(3000);
    alloc.deallocate(a, 7);
    alloc.deallocate(b, 200);
    alloc.deallocate(c, 3000);
}

TEST(MemoryAllocator, SingleBand) {
    utils::memory_allocator alloc(
        utils::size_class_config::from_bands({{8, 256}}));
    EXPECT_EQ(alloc.band_count(), 1u);
    EXPECT_EQ(alloc.size_class_count(), 32u);
    EXPECT_EQ(alloc.round_up_size(1), 8u);
    EXPECT_EQ(alloc.round_up_size(9), 16u);
    EXPECT_EQ(alloc.round_up_size(256), 256u);
    EXPECT_EQ(alloc.round_up_size(257), 0u);

    void* p = alloc.allocate(40);
    alloc.deallocate(p, 40);

    // 默认 max_classes=256：8B→4KiB 共 512 档会拒绝
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{8, 4096}})),
        std::invalid_argument);

    // max_classes=0：不设上限
    auto unlimited = utils::size_class_config::from_bands({{8, 4096}});
    unlimited.max_classes = 0;
    utils::memory_allocator dense(unlimited);
    EXPECT_EQ(dense.size_class_count(), 512u);
    EXPECT_EQ(dense.round_up_size(9), 16u);
    EXPECT_EQ(dense.round_up_size(4096), 4096u);
    void* q = dense.allocate(100);
    dense.deallocate(q, 100);
}

TEST(MemoryAllocator, ThreeBands) {
    utils::memory_allocator alloc(utils::size_class_config::from_bands(
        {{8, 64}, {32, 256}, {128, 1024}}));
    EXPECT_EQ(alloc.band_count(), 3u);
    EXPECT_EQ(alloc.round_up_size(7), 8u);
    EXPECT_EQ(alloc.round_up_size(64), 64u);
    EXPECT_EQ(alloc.round_up_size(65), 96u);
    EXPECT_EQ(alloc.round_up_size(256), 256u);
    EXPECT_EQ(alloc.round_up_size(257), 384u);
    EXPECT_EQ(alloc.round_up_size(1024), 1024u);
    EXPECT_EQ(alloc.round_up_size(1025), 0u);

    void* a = alloc.allocate(50);
    void* b = alloc.allocate(200);
    void* c = alloc.allocate(900);
    alloc.deallocate(a, 50);
    alloc.deallocate(b, 200);
    alloc.deallocate(c, 900);
}

TEST(MemoryAllocator, PresetAndCustomConfig) {
    utils::memory_allocator compact(utils::size_class_preset::compact);
    EXPECT_LT(compact.size_class_count(), 47u);
    EXPECT_EQ(compact.round_up_size(17), 32u);
    EXPECT_EQ(compact.round_up_size(129), 384u);

    utils::memory_allocator dense(utils::size_class_preset::dense_small);
    EXPECT_GT(dense.size_class_count(), 47u);
    EXPECT_EQ(dense.round_up_size(200), 200u);

    auto cfg = utils::size_class_config::from_two_level(8, 128, 256, 4224);
    utils::memory_allocator tuned(cfg);
    EXPECT_EQ(tuned.round_up_size(129), 384u);
    EXPECT_EQ(tuned.config().bands.size(), 2u);
    EXPECT_EQ(tuned.config().bands[1].step, 256u);

    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{0, 128}})),
        std::invalid_argument);
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{8, 128}, {128, 100}})),
        std::invalid_argument);
}

TEST(MemoryAllocator, ReusesFreelist) {
    utils::memory_allocator alloc;
    void* a = alloc.allocate(64);
    alloc.deallocate(a, 64);
    void* b = alloc.allocate(64);
    EXPECT_EQ(a, b);
    alloc.deallocate(b, 64);
}

TEST(TypedAlloc, AllocateByElementCount) {
    int* p = utils::typed_alloc<int>::allocate(4);
    ASSERT_NE(p, nullptr);
    p[0] = 1;
    p[3] = 4;
    EXPECT_EQ(p[0], 1);
    EXPECT_EQ(p[3], 4);
    utils::typed_alloc<int>::deallocate(p, 4);
}

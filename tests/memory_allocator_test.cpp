#include "memory/memory_allocator.h"
#include "memory/memory_pool.h"
#include "memory/memory_resource.h"
#include "memory/object_pool.h"
#include "memory/typed_alloc.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// =============================================================================
// size_class_config
// =============================================================================

TEST(SizeClassConfig, FromBandsAndTwoLevel) {
    auto bands = utils::size_class_config::from_bands({{8, 64}, {32, 256}});
    EXPECT_EQ(bands.bands.size(), 2u);
    EXPECT_EQ(bands.bands[0].step, 8u);
    EXPECT_EQ(bands.bands[0].max, 64u);
    EXPECT_EQ(bands.bands[1].step, 32u);
    EXPECT_EQ(bands.large_threshold, 0u);
    EXPECT_EQ(bands.refill_objects, 20u);
    EXPECT_EQ(bands.max_classes, 256u);

    auto two = utils::size_class_config::from_two_level(8, 128, 256, 4224);
    EXPECT_EQ(two.bands.size(), 2u);
    EXPECT_EQ(two.bands[0].step, 8u);
    EXPECT_EQ(two.bands[0].max, 128u);
    EXPECT_EQ(two.bands[1].step, 256u);
    EXPECT_EQ(two.bands[1].max, 4224u);
}

TEST(SizeClassConfig, FromPreset) {
    auto balanced =
        utils::size_class_config::from_preset(utils::size_class_preset::balanced);
    EXPECT_EQ(balanced.bands[0].step, 8u);
    EXPECT_EQ(balanced.bands[0].max, 128u);
    EXPECT_EQ(balanced.bands[1].step, 128u);
    EXPECT_EQ(balanced.bands[1].max, 4096u);

    auto dense = utils::size_class_config::from_preset(
        utils::size_class_preset::dense_small);
    EXPECT_EQ(dense.bands[0].max, 256u);

    auto compact =
        utils::size_class_config::from_preset(utils::size_class_preset::compact);
    EXPECT_EQ(compact.bands[0].step, 16u);
    EXPECT_EQ(compact.bands[1].step, 256u);
}

TEST(SizeClassConfig, RejectsInvalidBands) {
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({})),
        std::invalid_argument);
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{0, 128}})),
        std::invalid_argument);
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{8, 0}})),
        std::invalid_argument);
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{12, 128}})), // step 非 2 幂
        std::invalid_argument);
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{8, 128}, {128, 100}})),
        std::invalid_argument);
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{8, 128}, {8, 128}})),
        std::invalid_argument);

    auto bad_refill = utils::size_class_config::from_bands({{8, 128}});
    bad_refill.refill_objects = 0;
    EXPECT_THROW((void)utils::memory_allocator(bad_refill),
                 std::invalid_argument);

    // 默认 max_classes=256：8→4KiB 共 512 档会拒绝
    EXPECT_THROW(
        (void)utils::memory_allocator(
            utils::size_class_config::from_bands({{8, 4096}})),
        std::invalid_argument);
}

// =============================================================================
// memory_allocator — 构造 / 查询
// =============================================================================

TEST(MemoryAllocator, DefaultBalancedQueries) {
    utils::memory_allocator alloc;
    EXPECT_EQ(alloc.band_count(), 2u);
    EXPECT_EQ(alloc.size_class_count(), 47u);
    EXPECT_EQ(alloc.large_threshold(), 4096u);
    EXPECT_EQ(alloc.config().bands.size(), 2u);
    EXPECT_FALSE(alloc.class_sizes().empty());
    EXPECT_EQ(alloc.class_sizes().front(), 8u);
    EXPECT_EQ(alloc.class_sizes().back(), 4096u);
    EXPECT_EQ(alloc.class_sizes().size(), alloc.size_class_count());
}

TEST(MemoryAllocator, RoundUpSizeBoundaries) {
    utils::memory_allocator alloc;
    EXPECT_EQ(alloc.round_up_size(0), 8u); // 0 当作 1
    EXPECT_EQ(alloc.round_up_size(1), 8u);
    EXPECT_EQ(alloc.round_up_size(7), 8u);
    EXPECT_EQ(alloc.round_up_size(8), 8u);
    EXPECT_EQ(alloc.round_up_size(9), 16u);
    EXPECT_EQ(alloc.round_up_size(128), 128u);
    EXPECT_EQ(alloc.round_up_size(129), 256u);
    EXPECT_EQ(alloc.round_up_size(2049), 2176u);
    EXPECT_EQ(alloc.round_up_size(4096), 4096u);
    EXPECT_EQ(alloc.round_up_size(4097), 0u); // 超出池化档
}

TEST(MemoryAllocator, SingleBand) {
    utils::memory_allocator alloc(
        utils::size_class_config::from_bands({{8, 256}}));
    EXPECT_EQ(alloc.band_count(), 1u);
    EXPECT_EQ(alloc.size_class_count(), 32u);
    EXPECT_EQ(alloc.large_threshold(), 256u);
    EXPECT_EQ(alloc.round_up_size(1), 8u);
    EXPECT_EQ(alloc.round_up_size(9), 16u);
    EXPECT_EQ(alloc.round_up_size(256), 256u);
    EXPECT_EQ(alloc.round_up_size(257), 0u);

    void* p = alloc.allocate(40);
    ASSERT_NE(p, nullptr);
    alloc.deallocate(p, 40);
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

    utils::memory_allocator balanced(utils::size_class_preset::balanced);
    EXPECT_EQ(balanced.size_class_count(), 47u);

    auto cfg = utils::size_class_config::from_two_level(8, 128, 256, 4224);
    utils::memory_allocator tuned(cfg);
    EXPECT_EQ(tuned.round_up_size(129), 384u);
    EXPECT_EQ(tuned.config().bands.size(), 2u);
    EXPECT_EQ(tuned.config().bands[1].step, 256u);
}

TEST(MemoryAllocator, UnlimitedMaxClasses) {
    auto unlimited = utils::size_class_config::from_bands({{8, 4096}});
    unlimited.max_classes = 0;
    utils::memory_allocator dense(unlimited);
    EXPECT_EQ(dense.size_class_count(), 512u);
    EXPECT_EQ(dense.round_up_size(9), 16u);
    EXPECT_EQ(dense.round_up_size(4096), 4096u);
    void* q = dense.allocate(100);
    dense.deallocate(q, 100);
}

TEST(MemoryAllocator, CustomLargeThreshold) {
    auto cfg = utils::size_class_config::from_bands({{8, 128}});
    cfg.large_threshold = 64; // 小于最大档，但须 >= 最小档
    utils::memory_allocator alloc(cfg);
    EXPECT_EQ(alloc.large_threshold(), 64u);
    EXPECT_EQ(alloc.round_up_size(64), 64u);
    EXPECT_EQ(alloc.round_up_size(65), 0u);

    void* small = alloc.allocate(32);
    void* big = alloc.allocate(65); // 走 ::operator new
    ASSERT_NE(small, nullptr);
    ASSERT_NE(big, nullptr);
    alloc.deallocate(small, 32);
    alloc.deallocate(big, 65);
}

TEST(MemoryAllocator, RejectsTooSmallLargeThreshold) {
    auto cfg = utils::size_class_config::from_bands({{8, 128}});
    cfg.large_threshold = 4;
    EXPECT_THROW((void)utils::memory_allocator(cfg), std::invalid_argument);
}

// =============================================================================
// memory_allocator — allocate / deallocate
// =============================================================================

TEST(MemoryAllocator, AllocateZeroTreatedAsOne) {
    utils::memory_allocator alloc;
    void* p = alloc.allocate(0);
    ASSERT_NE(p, nullptr);
    alloc.deallocate(p, 0);
}

TEST(MemoryAllocator, SizeClassAndRawNew) {
    utils::memory_allocator alloc;
    void* small = alloc.allocate(24);
    void* big = alloc.allocate(alloc.large_threshold() + 1);
    ASSERT_NE(small, nullptr);
    ASSERT_NE(big, nullptr);
#if UTILS_POOL_LEAK_CHECK >= 1
    EXPECT_EQ(alloc.outstanding(), 2u);
    EXPECT_EQ(alloc.rounding_waste(), 0u); // 24 正好一档；大块走 new
    EXPECT_GT(alloc.class_table_bytes(), 0u);
#endif
    alloc.deallocate(small, 24);
    alloc.deallocate(big, alloc.large_threshold() + 1);
#if UTILS_POOL_LEAK_CHECK >= 1
    EXPECT_EQ(alloc.outstanding(), 0u);
#endif
}

TEST(MemoryAllocator, DeallocateNullptrIsNoop) {
    utils::memory_allocator alloc;
    alloc.deallocate(nullptr, 64);
#if UTILS_POOL_LEAK_CHECK >= 1
    EXPECT_EQ(alloc.outstanding(), 0u);
#endif
}

TEST(MemoryAllocator, DeallocateUncheckedReusesFreelist) {
    utils::memory_allocator alloc;
    void* a = alloc.allocate(64);
    alloc.deallocate_unchecked(a, 64);
    void* b = alloc.allocate(64);
    EXPECT_EQ(a, b);
    alloc.deallocate_unchecked(b, 64);
}

TEST(MemoryAllocator, ReusesFreelist) {
    utils::memory_allocator alloc;
    void* a = alloc.allocate(64);
    alloc.deallocate(a, 64);
    void* b = alloc.allocate(64);
    EXPECT_EQ(a, b);
    alloc.deallocate(b, 64);
}

TEST(MemoryAllocator, SameClassDifferentRequestsShareFreelist) {
    utils::memory_allocator alloc;
    // 9 与 16 同档 16
    void* a = alloc.allocate(9);
    alloc.deallocate(a, 9);
    void* b = alloc.allocate(16);
    EXPECT_EQ(a, b);
    alloc.deallocate(b, 16);
}

TEST(MemoryAllocator, MultipleClassesIndependent) {
    utils::memory_allocator alloc;
    void* a = alloc.allocate(8);
    void* b = alloc.allocate(32);
    void* c = alloc.allocate(128);
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    alloc.deallocate(a, 8);
    alloc.deallocate(b, 32);
    alloc.deallocate(c, 128);

    void* a2 = alloc.allocate(8);
    void* b2 = alloc.allocate(32);
    EXPECT_EQ(a2, a);
    EXPECT_EQ(b2, b);
    alloc.deallocate(a2, 8);
    alloc.deallocate(b2, 32);
}

TEST(MemoryAllocator, RefillProvidesMultipleObjects) {
    auto cfg = utils::size_class_config::from_bands({{8, 64}});
    cfg.refill_objects = 4;
    utils::memory_allocator alloc(cfg);

    std::vector<void*> ptrs;
    for (int i = 0; i < 8; ++i) {
        ptrs.push_back(alloc.allocate(16));
    }
    for (void* p : ptrs) {
        ASSERT_NE(p, nullptr);
        alloc.deallocate(p, 16);
    }
}

TEST(MemoryAllocator, TwoLevelSizeClassesAllocatePath) {
    utils::memory_allocator alloc;
    void* a = alloc.allocate(7);
    void* b = alloc.allocate(200);
    void* c = alloc.allocate(3000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    alloc.deallocate(a, 7);
    alloc.deallocate(b, 200);
    alloc.deallocate(c, 3000);
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
    EXPECT_EQ(alloc.class_table_bytes(), table0);
    EXPECT_EQ(alloc.outstanding(), 2u);
    alloc.deallocate(a, 7);
    alloc.deallocate(b, 25);
    EXPECT_EQ(alloc.outstanding(), 0u);
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

#if UTILS_POOL_LEAK_CHECK >= 2

TEST(MemoryAllocator, DumpLeaksTracksOutstandingAndSite) {
    utils::memory_allocator alloc;
    void* p = alloc.allocate(64);
    EXPECT_EQ(alloc.outstanding(), 1u);
    testing::internal::CaptureStderr();
    alloc.dump_leaks();
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("outstanding=1"), std::string::npos);
    EXPECT_NE(log.find("MemoryAllocator_DumpLeaksTracksOutstandingAndSite"),
              std::string::npos);
    alloc.deallocate(p, 64);
    EXPECT_EQ(alloc.outstanding(), 0u);
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

#endif  // UTILS_POOL_LEAK_CHECK >= 2

// =============================================================================
// byte_literals
// =============================================================================

TEST(ByteLiterals, PowersOfTwoUnits) {
    using namespace utils::byte_literals;
    EXPECT_EQ(1_KB, 1024u);
    EXPECT_EQ(4_KB, 4096u);
    EXPECT_EQ(1_MB, 1024u * 1024u);
    EXPECT_EQ(1_GB, 1024u * 1024u * 1024u);
    EXPECT_EQ(1_TB, 1024ull * 1024ull * 1024ull * 1024ull);

    utils::memory_allocator alloc(
        utils::size_class_config::from_bands({{8, 128}, {128, 4_KB}}));
    EXPECT_EQ(alloc.round_up_size(4_KB), 4_KB);
    EXPECT_EQ(alloc.round_up_size(4_KB + 1), 0u);
}

// =============================================================================
// typed_alloc（基于 memory_allocator）
// =============================================================================

TEST(TypedAlloc, AllocateByElementCount) {
    int* p = utils::typed_alloc<int>::allocate(4);
    ASSERT_NE(p, nullptr);
    p[0] = 1;
    p[3] = 4;
    EXPECT_EQ(p[0], 1);
    EXPECT_EQ(p[3], 4);
    utils::typed_alloc<int>::deallocate(p, 4);
}

TEST(TypedAlloc, ZeroCountReturnsNullptr) {
    EXPECT_EQ(utils::typed_alloc<int>::allocate(0), nullptr);
    utils::typed_alloc<int>::deallocate(nullptr, 0);
}

TEST(TypedAlloc, AllocatorAccessorIsPerThread) {
    auto& a = utils::typed_alloc<int>::allocator();
    auto& b = utils::typed_alloc<int>::allocator();
    EXPECT_EQ(&a, &b);
    void* p = a.allocate(16);
    ASSERT_NE(p, nullptr);
    a.deallocate(p, 16);
}

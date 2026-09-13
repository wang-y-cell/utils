#include "memory/object_pool.h"

#include <cstdint>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct tracked {
    static inline int alive = 0;
    int value;

    explicit tracked(int v) : value(v) { ++alive; }
    ~tracked() { --alive; }
};

struct multi_arg {
    int a;
    std::string b;
    multi_arg(int x, std::string y) : a(x), b(std::move(y)) {}
};

struct throwing {
    throwing() { throw std::runtime_error("boom"); }
};

struct throwing_dtor {
    static inline bool throw_on_dtor = false;
    ~throwing_dtor() noexcept(false) {
        if (throw_on_dtor) {
            throw std::runtime_error("dtor boom");
        }
    }
};

struct alignas(64) aligned_object {
    std::uint64_t values[8]{};
};

class ObjectPoolTest : public ::testing::Test {
protected:
    void SetUp() override { tracked::alive = 0; }
};

}  // namespace

TEST_F(ObjectPoolTest, CreateDestroyTracksLifetime) {
    utils::object_pool<tracked> pool(2);
    tracked* raw = pool.create(7);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->value, 7);
    EXPECT_EQ(tracked::alive, 1);
    pool.destroy(raw);
    EXPECT_EQ(tracked::alive, 0);
}

TEST_F(ObjectPoolTest, AcquireReleasesOnScopeExit) {
    utils::object_pool<tracked> pool(2);
    {
        auto owned = pool.acquire(9);
        EXPECT_EQ(owned->value, 9);
        EXPECT_EQ(tracked::alive, 1);
    }
    EXPECT_EQ(tracked::alive, 0);
}

TEST(ObjectPool, ConstructionFailureReturnsBlock) {
    utils::object_pool<throwing> failed(1);
    EXPECT_THROW((void)failed.create(), std::runtime_error);
    EXPECT_EQ(failed.in_use(), 0u);
}

TEST(ObjectPool, HonorsOverAlignment) {
    utils::object_pool<aligned_object> aligned(1);
    auto object = aligned.acquire();
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(object.get()) % 64, 0u);
    EXPECT_EQ(object->values[0], 0u);
}

TEST(ObjectPool, DestroyNullptrIsNoop) {
    utils::object_pool<tracked> pool(2);
    EXPECT_NO_THROW(pool.destroy(nullptr));
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ObjectPool, CreateMultiArgAndStorageStats) {
    utils::object_pool<multi_arg> pool(4);
    multi_arg* p = pool.create(3, std::string{"hi"});
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->a, 3);
    EXPECT_EQ(p->b, "hi");
    EXPECT_EQ(pool.in_use(), 1u);
    EXPECT_EQ(pool.storage().in_use(), 1u);
    EXPECT_EQ(pool.capacity(), pool.storage().capacity());
    EXPECT_EQ(pool.available(), pool.storage().available());
    pool.destroy(p);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ObjectPool, ThrowingDestructorStillReturnsBlock) {
    utils::object_pool<throwing_dtor> pool(2);
    throwing_dtor* p = pool.create();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.in_use(), 1u);
    throwing_dtor::throw_on_dtor = true;
    EXPECT_THROW(pool.destroy(p), std::runtime_error);
    throwing_dtor::throw_on_dtor = false;
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ObjectPool, DefaultDeleterNullPoolIsNoop) {
    utils::object_pool<tracked>::deleter d;
    EXPECT_NO_THROW(d(nullptr));
}

#if UTILS_POOL_LEAK_CHECK >= 2

TEST(ObjectPool, CreateAtAndAcquireAtRecordSite) {
    utils::object_pool<tracked> pool(2);
    tracked* raw = pool.create_at(std::source_location::current(), 1);
    ASSERT_NE(raw, nullptr);

    testing::internal::CaptureStderr();
    pool.dump_leaks();
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("outstanding=1"), std::string::npos);
    EXPECT_NE(log.find("ObjectPool_CreateAtAndAcquireAtRecordSite"),
              std::string::npos);

    pool.destroy(raw);
    {
        auto owned =
            pool.acquire_at(std::source_location::current(), 2);
        EXPECT_EQ(owned->value, 2);
        EXPECT_EQ(pool.in_use(), 1u);
    }
    EXPECT_EQ(pool.in_use(), 0u);
}

#endif  // UTILS_POOL_LEAK_CHECK >= 2

// =============================================================================
// object_pool — 压力
// =============================================================================

TEST(ObjectPoolStress, CreateDestroyChurn) {
    utils::object_pool<tracked> pool(64);
    constexpr int rounds = 100;
    constexpr int batch = 256;
    for (int r = 0; r < rounds; ++r) {
        std::vector<tracked*> ptrs;
        ptrs.reserve(batch);
        for (int i = 0; i < batch; ++i) {
            ptrs.push_back(pool.create(i));
        }
        EXPECT_EQ(tracked::alive, batch);
        for (tracked* p : ptrs) {
            pool.destroy(p);
        }
        EXPECT_EQ(tracked::alive, 0);
        EXPECT_EQ(pool.in_use(), 0u);
    }
}

TEST(ObjectPoolStress, AcquireUniquePtrChurn) {
    utils::object_pool<tracked> pool(32);
    for (int i = 0; i < 10000; ++i) {
        auto p = pool.acquire(i);
        EXPECT_EQ(p->value, i);
    }
    EXPECT_EQ(tracked::alive, 0);
    EXPECT_EQ(pool.in_use(), 0u);
}

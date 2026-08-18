#include "memory/object_pool.h"

#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {

struct tracked {
    static inline int alive = 0;
    int value;

    explicit tracked(int v) : value(v) { ++alive; }
    ~tracked() { --alive; }
};

struct throwing {
    throwing() { throw std::runtime_error("boom"); }
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

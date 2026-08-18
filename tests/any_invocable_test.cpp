#include "concurrency/detail/any_invocable.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include <gtest/gtest.h>

TEST(AnyInvocable, EmptyThrowsAndResetClears) {
    utils::any_invocable<void()> empty;
    EXPECT_FALSE(static_cast<bool>(empty));
    EXPECT_THROW(empty(), std::bad_function_call);

    empty = [] {};
    ASSERT_TRUE(static_cast<bool>(empty));
    empty();

    empty.reset();
    EXPECT_FALSE(static_cast<bool>(empty));
    empty = nullptr;
    EXPECT_FALSE(static_cast<bool>(empty));
}

TEST(AnyInvocable, InvokesAndReturnsValue) {
    utils::any_invocable<int(int)> add = [](int n) { return n + 1; };
    EXPECT_EQ(add(41), 42);
}

TEST(AnyInvocable, StoresMoveOnlyCallable) {
    auto owned = std::make_unique<int>(7);
    utils::any_invocable<int()> f = [owned = std::move(owned)] { return *owned; };
    EXPECT_EQ(f(), 7);

    utils::any_invocable<int()> moved = std::move(f);
    EXPECT_FALSE(static_cast<bool>(f));
    EXPECT_EQ(moved(), 7);
}

TEST(AnyInvocable, SwapExchangesTargets) {
    utils::any_invocable<int()> a = [] { return 1; };
    utils::any_invocable<int()> b = [] { return 2; };
    a.swap(b);
    EXPECT_EQ(a(), 2);
    EXPECT_EQ(b(), 1);
}

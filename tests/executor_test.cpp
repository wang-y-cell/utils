#include "concurrency/executor/adapters.h"
#include "concurrency/executor/executor.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <type_traits>

#include <gtest/gtest.h>

static_assert(utils::executor<utils::inline_executor>);
static_assert(utils::try_executor<utils::inline_executor>);
static_assert(utils::executor<utils::thread_pool_executor>);
static_assert(utils::try_executor<utils::thread_pool_executor>);
static_assert(utils::executor<utils::event_loop_executor>);
static_assert(utils::try_executor<utils::event_loop_executor>);
static_assert(utils::executor<utils::any_executor>);
static_assert(utils::try_executor<utils::any_executor>);

namespace {

template <class Pred>
bool wait_until(Pred pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

TEST(Executor, InlineRunsSynchronously) {
    utils::inline_executor exec;
    int value = 0;
    exec.post([&] { value = 1; });
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(exec.try_post([&] { value = 2; }));
    EXPECT_EQ(value, 2);

    auto made = utils::make_inline_executor();
    made.post([&] { value = 3; });
    EXPECT_EQ(value, 3);
}

TEST(Executor, EmptyAnyExecutorIsNoOp) {
    utils::any_executor empty;
    EXPECT_FALSE(static_cast<bool>(empty));
    int value = 0;
    empty.post([&] { value = 1; });
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(empty.try_post([&] { value = 2; }));
    EXPECT_EQ(value, 0);
}

TEST(Executor, AnyExecutorWrapsInlineAndThreadPool) {
    utils::inline_executor inline_ex;
    utils::any_executor any = inline_ex;
    EXPECT_TRUE(static_cast<bool>(any));
    int value = 0;
    any.post([&] { value = 7; });
    EXPECT_EQ(value, 7);

    utils::thread_pool pool(1);
    auto pool_ex = utils::make_executor(pool);
    EXPECT_EQ(pool_ex.target(), &pool);
    any = pool_ex;
    std::atomic<int> async{0};
    any.post([&] { async = 11; });
    any.post([owned = std::make_unique<int>(12), &async] { async = *owned; });
    pool.wait();
    EXPECT_EQ(async, 12);
}

TEST(Executor, ThreadPoolTryPostRespectsBackpressure) {
    utils::thread_pool pool(1, 1);
    auto exec = utils::make_executor(pool);

    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    ASSERT_TRUE(exec.try_post([&] {
        inside = true;
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    ASSERT_TRUE(wait_until([&] { return inside.load(); }));
    EXPECT_TRUE(exec.try_post([] {}));
    EXPECT_FALSE(exec.try_post([] {}));
    release = true;
    pool.wait();
}

TEST(Executor, EventLoopExecutorPostsUntilStopped) {
    utils::event_loop loop;
    auto exec = utils::make_executor(loop);
    EXPECT_EQ(exec.target(), &loop);

    int value = 0;
    ASSERT_TRUE(exec.try_post([&] { value = 9; }));
    loop.process_events();
    EXPECT_EQ(value, 9);

    loop.stop();
    EXPECT_FALSE(exec.try_post([&] { value = 10; }));
    EXPECT_EQ(value, 9);
}

TEST(Executor, EventLoopAdapterRunsMoveOnlyCallable) {
    utils::event_loop loop;
    auto exec = utils::make_executor(loop);
    int value = 0;
    exec.post([owned = std::make_unique<int>(5), &value] { value = *owned; });
    loop.process_events();
    EXPECT_EQ(value, 5);
}

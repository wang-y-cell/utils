#include "concurrency/thread_pool/thread_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

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

TEST(ThreadPool, ZeroCountBecomesOneAndSubmitReturnsValue) {
    utils::thread_pool pool(0);
    EXPECT_EQ(pool.target_thread_count(), 1u);
    ASSERT_TRUE(wait_until([&] { return pool.thread_count() >= 1; }));

    auto future = pool.submit([](int a, int b) { return a + b; }, 20, 22);
    EXPECT_EQ(future.get(), 42);
    EXPECT_FALSE(pool.stopped());
}

TEST(ThreadPool, SubmitPropagatesTaskException) {
    utils::thread_pool pool(1);
    auto future = pool.submit([]() -> int {
        throw std::runtime_error("task failed");
        return 0;
    });
    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST(ThreadPool, AddTaskExceptionsAreSwallowed) {
    utils::thread_pool pool(1);
    std::atomic<int> ran{0};
    pool.add_task([&] {
        ++ran;
        throw std::runtime_error("ignored");
    });
    pool.add_task([&] { ++ran; });
    pool.wait();
    EXPECT_EQ(ran, 2);
}

TEST(ThreadPool, TryAddTaskFailsWhenQueueFull) {
    utils::thread_pool pool(1, 1);

    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable go;
    bool worker_inside = false;
    bool release = false;

    ASSERT_TRUE(pool.try_add_task([&] {
        std::unique_lock<std::mutex> lock(mutex);
        worker_inside = true;
        ready.notify_one();
        go.wait(lock, [&] { return release; });
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return worker_inside; });
    }

    EXPECT_TRUE(pool.try_add_task([] {}));
    EXPECT_FALSE(pool.try_add_task([] {}));
    EXPECT_EQ(pool.pending_tasks(), 1u);

    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    go.notify_one();
    pool.wait();
    EXPECT_EQ(pool.pending_tasks(), 0u);
}

TEST(ThreadPool, ResizeUpAndDown) {
    utils::thread_pool pool(2);
    ASSERT_TRUE(wait_until([&] { return pool.thread_count() == 2; }));

    pool.resize(4);
    EXPECT_EQ(pool.target_thread_count(), 4u);
    ASSERT_TRUE(wait_until([&] { return pool.thread_count() == 4; }));

    pool.resize(1);
    EXPECT_EQ(pool.target_thread_count(), 1u);
    ASSERT_TRUE(wait_until([&] { return pool.thread_count() == 1; }));
}

TEST(ThreadPool, ShutdownRejectsNewWork) {
    utils::thread_pool pool(1);
    pool.shutdown(true);
    EXPECT_TRUE(pool.stopped());
    EXPECT_THROW(pool.submit([] {}), std::runtime_error);
    EXPECT_FALSE(pool.try_add_task([] {}));
}

TEST(ThreadPool, ShutdownCanDropQueuedTasks) {
    utils::thread_pool pool(1, 0);

    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable go;
    bool worker_inside = false;
    bool release = false;
    std::atomic<int> ran{0};

    pool.add_task([&] {
        std::unique_lock<std::mutex> lock(mutex);
        worker_inside = true;
        ready.notify_one();
        go.wait(lock, [&] { return release; });
        ++ran;
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return worker_inside; });
    }
    pool.add_task([&] { ++ran; });

    std::thread releaser([&] {
        while (!pool.stopped()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        go.notify_one();
    });
    pool.shutdown(false);
    releaser.join();

    EXPECT_EQ(ran, 1);
}

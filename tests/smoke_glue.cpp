#include "concurrency/executor/adapters.h"
#include "memory/memory.h"
#include "utils/utils.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using namespace utils;

TEST(Glue, Expected) {
    result<int> a = result_ok(21);
    auto b = a.transform([](int n) { return n * 2; });
    ASSERT_TRUE(b);
    EXPECT_EQ(*b, 42);

    result<int> c = result_err(std::errc::invalid_argument);
    auto d = c.or_else([](const std::error_code&) -> result<int> {
        return result_ok(7);
    });
    ASSERT_TRUE(d);
    EXPECT_EQ(*d, 7);

    expected<void, int> okv;
    EXPECT_TRUE(okv);
    expected<void, int> bad = utils::unexpected(3);
    EXPECT_FALSE(bad);
    EXPECT_EQ(bad.error(), 3);

    auto chained =
        result_ok(2).and_then([](int n) -> result<int> { return result_ok(n + 1); });
    EXPECT_EQ(chained.value_or(0), 3);
}

TEST(Glue, Executor) {
    inline_executor inline_ex;
    int x = 0;
    inline_ex.post([&] { x = 1; });
    EXPECT_EQ(x, 1);

    thread_pool pool(2);
    auto pool_ex = make_executor(pool);
    std::atomic<int> y{0};
    pool_ex.post([&] { y = 2; });
    pool_ex.post([owned = std::make_unique<int>(3), &y] { y = *owned; });
    pool.wait();
    EXPECT_EQ(y, 3);

    any_executor any = pool_ex;
    any.post([&] { y = 4; });
    pool.wait();
    EXPECT_EQ(y, 4);

    worker_thread worker;
    worker.start();
    for (int i = 0; i < 200 && !worker.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    event_loop* wloop = worker.loop();
    ASSERT_NE(wloop, nullptr);
    ASSERT_TRUE(worker.is_running());
    auto loop_ex = make_executor(*wloop);
    std::atomic<int> z{0};
    loop_ex.post([&] { z = 9; });
    for (int i = 0; i < 200 && z.load() != 9; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(z, 9);
    worker.stop();
}

TEST(Glue, Deadline) {
    auto d = deadline::after(std::chrono::milliseconds(30));
    EXPECT_FALSE(d.expired());
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_TRUE(d.expired());
    EXPECT_EQ(d.remaining().count(), 0);
}

TEST(Glue, Retry) {
    int calls = 0;
    auto r = retry(
        [&]() -> result<int> {
            ++calls;
            if (calls < 3) {
                return result_err(std::errc::connection_reset);
            }
            return result_ok(42);
        },
        retry_policy::fixed(5, std::chrono::milliseconds(1)));
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, 42);
    EXPECT_EQ(calls, 3);

    calls = 0;
    auto fail = retry(
        [&]() -> result<int> {
            ++calls;
            return result_err(std::errc::invalid_argument);
        },
        retry_policy::fixed(3, std::chrono::milliseconds(1)),
        [](const std::error_code& ec) {
            return ec != std::errc::invalid_argument;
        });
    EXPECT_FALSE(fail);
    EXPECT_EQ(calls, 1);
}

TEST(Glue, Channel) {
    channel<int> ch(2);
    EXPECT_TRUE(ch.send(1));
    EXPECT_TRUE(ch.send(2));
    EXPECT_FALSE(ch.try_send(3));

    auto a = ch.recv();
    ASSERT_TRUE(a);
    EXPECT_EQ(*a, 1);
    EXPECT_TRUE(ch.try_send(3));

    std::thread producer([&] {
        for (int i = 10; i < 15; ++i) {
            ch.send(i);
        }
        ch.close();
    });

    int sum = 0;
    while (auto v = ch.recv()) {
        sum += *v;
    }
    producer.join();
    EXPECT_EQ(sum, 2 + 3 + 10 + 11 + 12 + 13 + 14);
}

TEST(Glue, Memory) {
    object_pool<std::string> pool(2);
    auto text = pool.acquire("pooled");
    EXPECT_EQ(*text, "pooled");
    EXPECT_EQ(pool.in_use(), 1u);
}

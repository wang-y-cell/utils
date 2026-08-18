#include "concurrency/channel/channel.h"

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

TEST(Channel, UnboundedGrowsUntilRecv) {
    utils::channel<int> ch;
    EXPECT_EQ(ch.capacity(), 0u);
    EXPECT_TRUE(ch.send(1));
    EXPECT_TRUE(ch.send(2));
    EXPECT_TRUE(ch.try_send(3));
    EXPECT_EQ(ch.size(), 3u);
    EXPECT_FALSE(ch.closed());

    EXPECT_EQ(ch.try_recv().value(), 1);
    EXPECT_EQ(ch.recv().value(), 2);
    EXPECT_EQ(ch.recv().value(), 3);
    EXPECT_FALSE(ch.try_recv());
}

TEST(Channel, BoundedTrySendFailsWhenFull) {
    utils::channel<int> ch(2);
    EXPECT_TRUE(ch.try_send(1));
    EXPECT_TRUE(ch.try_send(2));
    EXPECT_FALSE(ch.try_send(3));
    EXPECT_EQ(ch.size(), 2u);

    EXPECT_EQ(ch.try_recv().value(), 1);
    EXPECT_TRUE(ch.try_send(3));
    EXPECT_EQ(ch.recv().value(), 2);
    EXPECT_EQ(ch.recv().value(), 3);
}

TEST(Channel, CloseRejectsSendAndDrainsThenNullopt) {
    utils::channel<std::string> ch(8);
    EXPECT_TRUE(ch.send("a"));
    EXPECT_TRUE(ch.send("b"));
    ch.close();
    EXPECT_TRUE(ch.closed());
    EXPECT_FALSE(ch.send("c"));
    EXPECT_FALSE(ch.try_send("d"));

    EXPECT_EQ(ch.recv().value(), "a");
    EXPECT_EQ(ch.try_recv().value(), "b");
    EXPECT_FALSE(ch.recv());
    EXPECT_FALSE(ch.try_recv());
}

TEST(Channel, RecvUnblocksWhenClosedEmpty) {
    utils::channel<int> ch(1);
    std::optional<int> got{0};
    std::thread consumer([&] { got = ch.recv(); });
    ch.close();
    consumer.join();
    EXPECT_FALSE(got.has_value());
}

TEST(Channel, SendUnblocksWhenSpaceFreed) {
    utils::channel<int> ch(1);
    ASSERT_TRUE(ch.send(1));

    std::atomic<bool> sent{false};
    std::thread producer([&] {
        EXPECT_TRUE(ch.send(2));
        sent = true;
    });

    EXPECT_EQ(ch.recv().value(), 1);
    producer.join();
    EXPECT_TRUE(sent);
    EXPECT_EQ(ch.recv().value(), 2);
}

TEST(Channel, MultiProducerSingleConsumer) {
    utils::channel<int> ch(16);
    constexpr int producers = 4;
    constexpr int per_producer = 50;

    std::vector<std::thread> workers;
    workers.reserve(producers);
    for (int p = 0; p < producers; ++p) {
        workers.emplace_back([&ch, p] {
            for (int i = 0; i < per_producer; ++i) {
                EXPECT_TRUE(ch.send(p * per_producer + i));
            }
        });
    }

    int sum = 0;
    for (int i = 0; i < producers * per_producer; ++i) {
        auto v = ch.recv();
        ASSERT_TRUE(v);
        sum += *v;
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const int n = producers * per_producer;
    EXPECT_EQ(sum, (n - 1) * n / 2);
}

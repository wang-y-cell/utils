#include "reliability/time/deadline.h"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

TEST(Deadline, NeverDoesNotExpire) {
    auto never = utils::deadline::never();
    EXPECT_FALSE(never.expired());
    EXPECT_GT(never.remaining().count(), 0);
    EXPECT_EQ(never.time_point_value(), utils::deadline::clock::time_point::max());
}

TEST(Deadline, AfterExpiresAndRemainingGoesToZero) {
    auto d = utils::deadline::after(std::chrono::milliseconds(30));
    EXPECT_FALSE(d.expired());
    EXPECT_GT(d.remaining_as<std::chrono::milliseconds>().count(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_TRUE(d.expired());
    EXPECT_EQ(d.remaining().count(), 0);
    EXPECT_EQ(d.remaining_as<std::chrono::milliseconds>().count(), 0);
}

TEST(Deadline, AtPastIsAlreadyExpired) {
    auto past = utils::deadline::at(utils::deadline::clock::now() -
                                    std::chrono::milliseconds(5));
    EXPECT_TRUE(past.expired());
    EXPECT_EQ(past.remaining().count(), 0);
}

TEST(Deadline, DefaultConstructedMatchesNever) {
    utils::deadline d;
    EXPECT_FALSE(d.expired());
}

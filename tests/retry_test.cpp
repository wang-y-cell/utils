#include "reliability/retry/retry.h"

#include <chrono>
#include <stop_token>
#include <system_error>

#include <gtest/gtest.h>

TEST(RetryPolicy, FixedDelayThenExhausts) {
    auto policy = utils::retry_policy::fixed(3, std::chrono::milliseconds(10));
    auto first = policy.delay_after_failure(0);
    auto second = policy.delay_after_failure(1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, std::chrono::milliseconds(10));
    EXPECT_EQ(*second, std::chrono::milliseconds(10));
    EXPECT_FALSE(policy.delay_after_failure(2));
}

TEST(RetryPolicy, ExponentialGrowsUntilMax) {
    utils::retry_policy policy;
    policy.max_attempts = 5;
    policy.initial_delay = std::chrono::milliseconds(10);
    policy.multiplier = 2.0;
    policy.max_delay = std::chrono::milliseconds(30);
    policy.jitter = false;

    EXPECT_EQ(*policy.delay_after_failure(0), std::chrono::milliseconds(10));
    EXPECT_EQ(*policy.delay_after_failure(1), std::chrono::milliseconds(20));
    EXPECT_EQ(*policy.delay_after_failure(2), std::chrono::milliseconds(30));
}

TEST(Retry, SucceedsAfterTransientFailures) {
    int calls = 0;
    auto result = utils::retry(
        [&]() -> utils::result<int> {
            ++calls;
            if (calls < 3) {
                return utils::result_err(std::errc::connection_reset);
            }
            return utils::result_ok(42);
        },
        utils::retry_policy::fixed(5, std::chrono::milliseconds(1)));
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 42);
    EXPECT_EQ(calls, 3);
}

TEST(Retry, PredicateStopsWithoutRetrying) {
    int calls = 0;
    auto result = utils::retry(
        [&]() -> utils::result<int> {
            ++calls;
            return utils::result_err(std::errc::invalid_argument);
        },
        utils::retry_policy::fixed(4, std::chrono::milliseconds(1)),
        [](const std::error_code& ec) {
            return ec != std::make_error_code(std::errc::invalid_argument);
        });
    EXPECT_FALSE(result);
    EXPECT_EQ(calls, 1);
}

TEST(Retry, StopTokenCancelsBeforeFirstAttempt) {
    std::stop_source source;
    source.request_stop();
    int calls = 0;
    auto result = utils::retry(
        [&]() -> utils::result<int> {
            ++calls;
            return utils::result_ok(1);
        },
        utils::retry_policy::fixed(3, std::chrono::milliseconds(1)),
        source.get_token());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error(),
              std::make_error_code(std::errc::operation_canceled));
    EXPECT_EQ(calls, 0);
}

TEST(Retry, AlreadyExpiredDeadlineCancelsWithoutCalling) {
    int calls = 0;
    auto result = utils::retry(
        [&]() -> utils::result<int> {
            ++calls;
            return utils::result_ok(1);
        },
        utils::retry_policy::fixed(3, std::chrono::milliseconds(1)), {},
        utils::deadline::at(utils::deadline::clock::now() -
                            std::chrono::seconds(1)));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error(),
              std::make_error_code(std::errc::operation_canceled));
    EXPECT_EQ(calls, 0);
}

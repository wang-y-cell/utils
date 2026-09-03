#include "reliability/result/expected.h"

#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

TEST(Expected, ResultOkAndErr) {
    utils::result<int> success = 42;
    ASSERT_TRUE(success);
    EXPECT_TRUE(success.has_value());
    EXPECT_EQ(*success, 42);
    EXPECT_EQ(success.value(), 42);
    EXPECT_EQ(success.value_or(0), 42);

    utils::result<int> err = utils::err(std::errc::invalid_argument);
    EXPECT_FALSE(err);
    EXPECT_EQ(err.error(), std::make_error_code(std::errc::invalid_argument));
    EXPECT_EQ(err.value_or(7), 7);
    EXPECT_EQ(err.error_or(std::make_error_code(std::errc::io_error)),
              std::make_error_code(std::errc::invalid_argument));
    EXPECT_THROW(err.value(), utils::bad_expected_access);
}

TEST(Expected, VoidSuccessAndFailure) {
    utils::expected<void, int> success;
    EXPECT_TRUE(success);
    success.value();

    utils::expected<void, int> bad = utils::unexpected(3);
    EXPECT_FALSE(bad);
    EXPECT_EQ(bad.error(), 3);
    EXPECT_THROW(bad.value(), utils::bad_expected_access);

    utils::result<void> ok_result{};
    EXPECT_TRUE(ok_result);
}

TEST(Expected, TransformAndThenOrElse) {
    auto doubled = utils::result<int>{21}.transform([](int n) { return n * 2; });
    ASSERT_TRUE(doubled);
    EXPECT_EQ(*doubled, 42);

    utils::result<int> failed = utils::err(std::errc::io_error);
    auto skipped = failed.transform([](int n) { return n * 2; });
    EXPECT_FALSE(skipped);
    EXPECT_EQ(skipped.error(), std::make_error_code(std::errc::io_error));

    auto chained =
        utils::result<int>{2}.and_then([](int n) -> utils::result<int> {
            return n + 1;
        });
    EXPECT_EQ(chained.value_or(0), 3);

    auto recovered = failed.or_else([](const std::error_code&) -> utils::result<int> {
        return 8;
    });
    ASSERT_TRUE(recovered);
    EXPECT_EQ(*recovered, 8);
}

TEST(Expected, CopyMoveAndUnexpected) {
    utils::expected<std::string, int> original(std::in_place, "hi");
    auto copied = original;
    ASSERT_TRUE(copied);
    EXPECT_EQ(*copied, "hi");

    auto moved = std::move(original);
    ASSERT_TRUE(moved);
    EXPECT_EQ(*moved, "hi");

    utils::result<int> from_unexpected = utils::err(std::errc::timed_out);
    EXPECT_FALSE(from_unexpected);
    EXPECT_EQ(from_unexpected.error(), std::make_error_code(std::errc::timed_out));
}

TEST(Expected, CustomErrorType) {
    utils::result<int, std::string> success = 42;
    ASSERT_TRUE(success);
    EXPECT_EQ(*success, 42);

    utils::result<int, std::string> err = utils::err(std::string{"bad"});
    EXPECT_FALSE(err);
    EXPECT_EQ(err.error(), "bad");
    EXPECT_EQ(err.value_or(0), 0);

    utils::result<void, std::string> void_ok{};
    EXPECT_TRUE(void_ok);

    utils::result<void, std::string> void_err = utils::err(std::string{"nope"});
    EXPECT_FALSE(void_err);
    EXPECT_EQ(void_err.error(), "nope");

    auto chained =
        utils::result<int, std::string>{2}.and_then(
            [](int n) -> utils::result<int, std::string> {
                return n + 1;
            });
    ASSERT_TRUE(chained);
    EXPECT_EQ(*chained, 3);
}

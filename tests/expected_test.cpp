#include "reliability/result/expected.h"

#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

TEST(Expected, ResultOkAndErr) {
    utils::result<int> ok = utils::result_ok(42);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 42);
    EXPECT_EQ(ok.value(), 42);
    EXPECT_EQ(ok.value_or(0), 42);

    utils::result<int> err = utils::result_err(std::errc::invalid_argument);
    EXPECT_FALSE(err);
    EXPECT_EQ(err.error(), std::make_error_code(std::errc::invalid_argument));
    EXPECT_EQ(err.value_or(7), 7);
    EXPECT_EQ(err.error_or(std::make_error_code(std::errc::io_error)),
              std::make_error_code(std::errc::invalid_argument));
    EXPECT_THROW(err.value(), utils::bad_expected_access);
}

TEST(Expected, VoidSuccessAndFailure) {
    utils::expected<void, int> ok;
    EXPECT_TRUE(ok);
    ok.value();

    utils::expected<void, int> bad = utils::unexpected(3);
    EXPECT_FALSE(bad);
    EXPECT_EQ(bad.error(), 3);
    EXPECT_THROW(bad.value(), utils::bad_expected_access);

    utils::result<void> ok_result = utils::result_ok();
    EXPECT_TRUE(ok_result);
}

TEST(Expected, TransformAndThenOrElse) {
    auto doubled = utils::result_ok(21).transform([](int n) { return n * 2; });
    ASSERT_TRUE(doubled);
    EXPECT_EQ(*doubled, 42);

    utils::result<int> failed = utils::result_err(std::errc::io_error);
    auto skipped = failed.transform([](int n) { return n * 2; });
    EXPECT_FALSE(skipped);
    EXPECT_EQ(skipped.error(), std::make_error_code(std::errc::io_error));

    auto chained =
        utils::result_ok(2).and_then([](int n) -> utils::result<int> {
            return utils::result_ok(n + 1);
        });
    EXPECT_EQ(chained.value_or(0), 3);

    auto recovered = failed.or_else([](const std::error_code&) -> utils::result<int> {
        return utils::result_ok(8);
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

    utils::result<int> from_unexpected = utils::result_err(std::errc::timed_out);
    EXPECT_FALSE(from_unexpected);
    EXPECT_EQ(from_unexpected.error(), std::make_error_code(std::errc::timed_out));
}

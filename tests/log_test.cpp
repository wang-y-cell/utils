#include "facade/log/log.h"

#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct captured_entry {
    utils::log::level level;
    std::string message;
};

class capturing_backend final : public utils::log::backend {
public:
    void log(utils::log::level level, std::string_view message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back({level, std::string(message)});
    }

    std::vector<captured_entry> entries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<captured_entry> entries_;
};

class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        previous_level_ = utils::log::get_level();
        capture_ = std::make_shared<capturing_backend>();
        utils::log::set_backend(capture_);
        utils::log::set_level(utils::log::level::trace);
    }

    void TearDown() override {
        utils::log::set_backend(std::make_shared<utils::log::stream_backend>());
        utils::log::set_level(previous_level_);
    }

    std::shared_ptr<capturing_backend> capture_;
    utils::log::level previous_level_{};
};

}  // namespace

TEST_F(LogTest, WritesFormattedMessagesAtEachLevel) {
    utils::log::trace("t={}", 1);
    utils::log::debug("d={}", 2);
    utils::log::info("port={}", 8080);
    utils::log::warn("w={}", 3);
    utils::log::error("e={}", 4);

    auto entries = capture_->entries();
    ASSERT_EQ(entries.size(), 5u);
    EXPECT_EQ(entries[0].level, utils::log::level::trace);
    EXPECT_EQ(entries[2].message, "port=8080");
    EXPECT_EQ(entries[4].level, utils::log::level::error);
}

TEST_F(LogTest, FiltersBelowConfiguredLevel) {
    utils::log::set_level(utils::log::level::warn);
    EXPECT_EQ(utils::log::get_level(), utils::log::level::warn);

    utils::log::info("ignored");
    utils::log::warn("kept");
    utils::log::error("also");

    auto entries = capture_->entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].message, "kept");
    EXPECT_EQ(entries[1].message, "also");
}

TEST_F(LogTest, OffDropsEverything) {
    utils::log::set_level(utils::log::level::off);
    utils::log::error("nope");
    EXPECT_TRUE(capture_->entries().empty());
}

TEST_F(LogTest, NullBackendAndNullptrReplacementAreSafe) {
    utils::log::set_backend(std::make_shared<utils::log::null_backend>());
    utils::log::info("silent");
    EXPECT_TRUE(capture_->entries().empty());

    utils::log::set_backend(nullptr);
    utils::log::error("still silent");
}

TEST(LogStreamBackend, WritesBracketedLevelToStream) {
    std::ostringstream out;
    utils::log::stream_backend backend(out);
    backend.log(utils::log::level::info, "hello");
    EXPECT_EQ(out.str(), "[INFO] hello\n");
}

#include "facade/json/value.h"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

TEST(JsonValue, ConstructsScalars) {
    using utils::json::kind;
    using utils::json::value;

    EXPECT_EQ(value{}.type(), kind::null);
    EXPECT_TRUE(value{true}.as_bool().value());
    EXPECT_EQ(value{std::int64_t{42}}.as_int().value(), 42);
    EXPECT_DOUBLE_EQ(value{1.5}.as_double().value(), 1.5);
    EXPECT_EQ(value{"text"}.as_string().value(), "text");
}

TEST(JsonValue, ObjectPathIsValueSemantic) {
    using utils::json::value;

    auto root = value::object();
    ASSERT_TRUE(root.set("db.host", value{"localhost"}));
    ASSERT_TRUE(root.get("db.host"));
    EXPECT_EQ(root.get("db.host")->as_string().value(), "localhost");

    value copied = root;
    ASSERT_TRUE(copied.set("db.host", value{"other"}));
    EXPECT_EQ(root.get("db.host")->as_string().value(), "localhost");
}

TEST(JsonValue, ArrayPushRejectsObjects) {
    using utils::json::value;

    auto array = value::array();
    ASSERT_TRUE(array.push_back(value{std::int64_t{1}}));
    EXPECT_EQ(array.size(), 1u);

    auto root = value::object();
    EXPECT_FALSE(root.push_back(value{}));
    EXPECT_FALSE(root.get("missing"));
}

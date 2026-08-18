#include "facade/json/json.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

class fake_backend final : public utils::json::backend {
public:
    utils::result<utils::json::value> parse(std::string_view) override {
        auto output = utils::json::value::object();
        output.set("backend", utils::json::value{"fake"});
        return utils::result_ok(std::move(output));
    }

    utils::result<std::string> stringify(const utils::json::value&,
                                         bool) override {
        return utils::result_ok(std::string{"fake"});
    }
};

class throwing_backend final : public utils::json::backend {
public:
    utils::result<utils::json::value> parse(std::string_view) override {
        throw std::runtime_error("parse failure");
    }

    utils::result<std::string> stringify(const utils::json::value&,
                                         bool) override {
        throw std::runtime_error("stringify failure");
    }
};

class JsonBackendTest : public ::testing::Test {
protected:
    void TearDown() override { utils::json::reset_backend(); }
};

}  // namespace

TEST(JsonFacade, ParseGetSetAndPrettyStringify) {
    auto doc =
        utils::json::parse(R"({"port":8080,"db":{"host":"localhost"}})");
    ASSERT_TRUE(doc);

    auto port = doc->get("port");
    ASSERT_TRUE(port);
    EXPECT_EQ(port->as_int().value(), 8080);

    auto host = doc->get("db.host");
    ASSERT_TRUE(host);
    EXPECT_EQ(host->as_string().value(), "localhost");

    ASSERT_TRUE(doc->set("db.pool", utils::json::value(std::int64_t{4})));
    auto pool = doc->get("db.pool");
    ASSERT_TRUE(pool);
    EXPECT_EQ(pool->as_int().value(), 4);

    auto array = utils::json::value::array();
    ASSERT_TRUE(array.push_back(utils::json::value("x")));
    EXPECT_EQ(array.size(), 1u);

    auto text = utils::json::stringify(*doc, true);
    ASSERT_TRUE(text);
    EXPECT_NE(text->find("\"pool\""), std::string::npos);

    EXPECT_FALSE(utils::json::parse("{"));
    EXPECT_FALSE(host->as_int());
    EXPECT_FALSE(doc->get("missing"));
    EXPECT_FALSE(doc->push_back(utils::json::value(std::int64_t{1})));
}

TEST(JsonFacade, ParsesCompleteDocumentAndRoundTrips) {
    auto complete = utils::json::parse(
        R"({"null":null,"bool":true,"int":-12,"float":1.25e2,"array":[1,"x"],"unicode":"\u4F60\u597D \uD83D\uDE00"})");
    ASSERT_TRUE(complete);
    EXPECT_EQ(complete->get("null")->type(), utils::json::kind::null);
    EXPECT_TRUE(complete->get("bool")->as_bool().value());
    EXPECT_EQ(complete->get("int")->as_int().value(), -12);
    EXPECT_DOUBLE_EQ(complete->get("float")->as_double().value(), 125.0);
    EXPECT_EQ(complete->get("unicode")->as_string().value(), "你好 😀");

    auto pretty = utils::json::stringify(*complete, true);
    ASSERT_TRUE(pretty);
    EXPECT_NE(pretty->find("\n  \""), std::string::npos);
    auto round_trip = utils::json::parse(*pretty);
    ASSERT_TRUE(round_trip);
    EXPECT_EQ(round_trip->get("unicode")->as_string().value(), "你好 😀");
}

TEST(JsonFacade, RejectsInvalidInput) {
    EXPECT_FALSE(utils::json::parse(R"({"x":1,})"));
    EXPECT_FALSE(utils::json::parse("\"\\u" "D800\""));
    EXPECT_FALSE(utils::json::parse("01"));
    EXPECT_FALSE(utils::json::parse("1."));
    EXPECT_FALSE(utils::json::parse("1e"));
    EXPECT_FALSE(utils::json::parse("+1"));
    EXPECT_FALSE(utils::json::parse("1e400"));
    EXPECT_FALSE(utils::json::parse("true trailing"));
    EXPECT_FALSE(utils::json::parse("\"line\nbreak\""));
    EXPECT_FALSE(utils::json::parse("\"\\u" "DC00\""));

    std::string invalid_utf8{"\""};
    invalid_utf8.push_back(static_cast<char>(0xC0));
    invalid_utf8.push_back(static_cast<char>(0xAF));
    invalid_utf8.push_back('"');
    EXPECT_FALSE(utils::json::parse(invalid_utf8));
}

TEST(JsonFacade, EnforcesNestingLimit) {
    std::string deep;
    for (int i = 0; i < 257; ++i) deep.push_back('[');
    for (int i = 0; i < 257; ++i) deep.push_back(']');
    EXPECT_FALSE(utils::json::parse(deep));

    std::string maximum_depth;
    for (int i = 0; i < 256; ++i) maximum_depth.push_back('[');
    for (int i = 0; i < 256; ++i) maximum_depth.push_back(']');
    EXPECT_TRUE(utils::json::parse(maximum_depth));
}

TEST(JsonFacade, NumberAndUtf8Serialization) {
    auto large_integer = utils::json::parse("9223372036854775808");
    ASSERT_TRUE(large_integer);
    EXPECT_EQ(large_integer->type(), utils::json::kind::number);

    utils::json::value floating{1.0};
    auto floating_text = utils::json::stringify(floating);
    ASSERT_TRUE(floating_text);
    auto floating_round_trip = utils::json::parse(*floating_text);
    ASSERT_TRUE(floating_round_trip);
    EXPECT_EQ(floating_round_trip->type(), utils::json::kind::number);

    EXPECT_FALSE(utils::json::stringify(utils::json::value{
        std::numeric_limits<double>::infinity()}));
    std::string invalid_value;
    invalid_value.push_back(static_cast<char>(0xFF));
    EXPECT_FALSE(utils::json::stringify(utils::json::value{
        std::move(invalid_value)}));
}

TEST_F(JsonBackendTest, ReplacesAndRestoresDefaultBackend) {
    utils::json::set_backend(std::make_shared<fake_backend>());
    EXPECT_EQ(utils::json::parse("ignored")
                  ->get("backend")
                  ->as_string()
                  .value(),
              "fake");
    EXPECT_EQ(utils::json::stringify(utils::json::value{}).value(), "fake");

    utils::json::reset_backend();
    EXPECT_TRUE(utils::json::parse(R"({"restored":true})"));
    utils::json::set_backend(nullptr);
    EXPECT_TRUE(utils::json::parse(R"({"restored_again":true})"));
}

TEST_F(JsonBackendTest, ConvertsBackendExceptionsToResult) {
    utils::json::set_backend(std::make_shared<throwing_backend>());
    EXPECT_FALSE(utils::json::parse("ignored"));
    EXPECT_FALSE(utils::json::stringify(utils::json::value{}));
}

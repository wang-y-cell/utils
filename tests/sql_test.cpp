#include "facade/sql/sql.h"

#include <string>
#include <system_error>

#include <gtest/gtest.h>

TEST(Sql, CannedQueryAndTransactionLifecycle) {
    utils::sql::memory_db db;
    db.on_query("SELECT 1", utils::sql::result_set{{"n"}, {{std::string{"1"}}}});

    auto opened = db.open();
    ASSERT_TRUE(opened);
    auto conn = opened.value();
    ASSERT_TRUE(conn);

    auto query = conn->query("SELECT 1");
    ASSERT_TRUE(query);
    ASSERT_EQ(query->columns.size(), 1u);
    ASSERT_EQ(query->rows.size(), 1u);
    ASSERT_TRUE(query->rows[0][0]);
    EXPECT_EQ(*query->rows[0][0], "1");

    EXPECT_FALSE(conn->in_transaction());
    ASSERT_TRUE(conn->begin());
    EXPECT_TRUE(conn->in_transaction());
    EXPECT_FALSE(conn->begin());
    ASSERT_TRUE(conn->commit());
    EXPECT_FALSE(conn->in_transaction());
    EXPECT_FALSE(conn->commit());
    EXPECT_FALSE(conn->rollback());

    ASSERT_TRUE(conn->begin());
    ASSERT_TRUE(conn->rollback());
    EXPECT_FALSE(conn->in_transaction());

    const auto& log = db.log();
    ASSERT_GE(log.size(), 4u);
    EXPECT_EQ(log[0], "SELECT 1");
    EXPECT_EQ(log[1], "BEGIN");
    EXPECT_EQ(log[2], "COMMIT");
    EXPECT_EQ(log[3], "BEGIN");
}

TEST(Sql, QueryHandlerSeesParamsAndUnmatchedIsEmpty) {
    utils::sql::memory_db db;
    db.on_query("SELECT ?", [](const utils::sql::params& params) {
        utils::sql::result_set set;
        set.columns = {"v"};
        set.rows.push_back({params.empty() ? std::nullopt : params[0]});
        return set;
    });

    auto conn = db.open().value();
    auto matched = conn->query("SELECT ?", {std::string{"x"}});
    ASSERT_TRUE(matched);
    ASSERT_EQ(matched->rows.size(), 1u);
    ASSERT_TRUE(matched->rows[0][0]);
    EXPECT_EQ(*matched->rows[0][0], "x");

    auto missing = conn->query("SELECT missing");
    ASSERT_TRUE(missing);
    EXPECT_TRUE(missing->rows.empty());
}

TEST(Sql, ExecHandlerCanFailAndDefaultExecSucceeds) {
    utils::sql::memory_db db;
    db.on_exec("FAIL", [](const utils::sql::params&) -> utils::result<void> {
        return utils::result_err(std::errc::io_error);
    });

    auto conn = db.open().value();
    auto failed = conn->exec("FAIL");
    EXPECT_FALSE(failed);
    EXPECT_EQ(failed.error(), std::make_error_code(std::errc::io_error));

    auto ok = conn->exec("INSERT 1");
    EXPECT_TRUE(ok);
    EXPECT_EQ(db.log().back(), "INSERT 1");
}

#include "facade/sql/sql.h"

#include <iostream>
#include <memory>

using namespace utils;

int main() {
    auto db = std::make_shared<sql::memory_db>();
    db->on_query("SELECT id FROM t WHERE name=?",
                 sql::result_set{{"id"}, {{std::string{"7"}}}});
    db->on_exec("INSERT INTO t(name) VALUES(?)",
                [](const sql::params&) { return result_ok(); });

    auto connection = db->open();
    auto inserted = connection.value()->exec(
        "INSERT INTO t(name) VALUES(?)", {std::string{"alice"}});
    auto rows = connection.value()->query(
        "SELECT id FROM t WHERE name=?", {std::string{"alice"}});

    std::cout << "exec ok=" << inserted.has_value()
              << " id=" << rows.value().rows.at(0).at(0).value_or("-")
              << '\n';
}

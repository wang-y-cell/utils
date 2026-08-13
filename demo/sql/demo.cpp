/**
 * sql 关系库门面
 * 编译: cmake --build build --target demo_sql
 *
 * 要点:
 * - 业务只调 exec / query / begin / commit / rollback
 * - memory_db 按 SQL 字符串脚本化，适合单测
 * - 真库：实现 sql::connection，启动时注入
 */

#include "adapter/sql/sql.h"

#include <iostream>
#include <memory>

using namespace utils;

int main() {
    std::cout << "=== memory_db ===\n";
    auto db = std::make_shared<sql::memory_db>();
    db->on_query("SELECT id FROM t WHERE name=?",
                 sql::result_set{{"id"}, {{std::string{"7"}}}});
    db->on_exec("INSERT INTO t(name) VALUES(?)",
                [](const sql::params&) { return result_ok(); });

    auto conn = db->open();
    auto ins = conn.value()->exec("INSERT INTO t(name) VALUES(?)",
                                  {std::string{"alice"}});
    auto q = conn.value()->query("SELECT id FROM t WHERE name=?",
                                 {std::string{"alice"}});
    std::cout << "  exec ok=" << ins.has_value()
              << " id=" << q.value().rows.at(0).at(0).value_or("-") << '\n';

    auto tx = conn.value()->begin();
    std::cout << "  in_tx=" << conn.value()->in_transaction() << '\n';
    (void)conn.value()->commit();
    std::cout << "  after commit in_tx=" << conn.value()->in_transaction()
              << '\n';

    std::cout << "  log:";
    for (const auto& s : db->log()) {
        std::cout << " [" << s << "]";
    }
    std::cout << '\n';

    std::cout << "\ndemo_sql: ok\n";
    return 0;
}

#pragma once

/**
 * sql — 关系库薄门面（不绑 SOCI / libpqxx；勿做成 ORM）
 *
 *   auto db = std::make_shared<utils::sql::memory_db>();
 *   db->on_query("SELECT 1", {{"n"}, {{"1"}}});
 *   auto conn = db->open();
 *   auto rows = conn->query("SELECT 1");
 *
 * 真后端：实现 connection（exec / query / begin / commit / rollback）。
 */

#include "component/result/expected.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace utils::sql {

using value = std::optional<std::string>;
using row = std::vector<value>;
using params = std::vector<value>;

struct result_set {
    std::vector<std::string> columns;
    std::vector<row> rows;
};

class connection {
public:
    virtual ~connection() = default;

    virtual result<void> exec(std::string_view sql, const params& p = {}) = 0;
    virtual result<result_set> query(std::string_view sql,
                                     const params& p = {}) = 0;
    virtual result<void> begin() = 0;
    virtual result<void> commit() = 0;
    virtual result<void> rollback() = 0;
    [[nodiscard]] virtual bool in_transaction() const = 0;
};

class database {
public:
    virtual ~database() = default;
    virtual result<std::shared_ptr<connection>> open() = 0;
};

/** 脚本化内存库：按 SQL 全文匹配 handler；事务只记标志，不回滚数据 */
class memory_db : public database {
public:
    using query_handler = std::function<result_set(const params&)>;
    using exec_handler = std::function<result<void>(const params&)>;

    void on_query(std::string sql, result_set canned) {
        queries_[std::move(sql)] = [c = std::move(canned)](const params&) {
            return c;
        };
    }

    void on_query(std::string sql, query_handler h) {
        queries_[std::move(sql)] = std::move(h);
    }

    void on_exec(std::string sql, exec_handler h) {
        execs_[std::move(sql)] = std::move(h);
    }

    result<std::shared_ptr<connection>> open() override {
        std::shared_ptr<connection> c = std::make_shared<conn>(this);
        return result_ok(std::move(c));
    }

    [[nodiscard]] const std::vector<std::string>& log() const { return log_; }

private:
    class conn final : public connection {
    public:
        explicit conn(memory_db* db) : db_(db) {}

        result<void> exec(std::string_view sql, const params& p = {}) override {
            db_->log_.emplace_back(sql);
            auto it = db_->execs_.find(std::string(sql));
            if (it != db_->execs_.end()) {
                return it->second(p);
            }
            return result_ok();
        }

        result<result_set> query(std::string_view sql,
                                 const params& p = {}) override {
            db_->log_.emplace_back(sql);
            auto it = db_->queries_.find(std::string(sql));
            if (it != db_->queries_.end()) {
                return result_ok(it->second(p));
            }
            return result_ok(result_set{});
        }

        result<void> begin() override {
            if (in_tx_) {
                return result_err(std::errc::invalid_argument);
            }
            in_tx_ = true;
            db_->log_.emplace_back("BEGIN");
            return result_ok();
        }

        result<void> commit() override {
            if (!in_tx_) {
                return result_err(std::errc::invalid_argument);
            }
            in_tx_ = false;
            db_->log_.emplace_back("COMMIT");
            return result_ok();
        }

        result<void> rollback() override {
            if (!in_tx_) {
                return result_err(std::errc::invalid_argument);
            }
            in_tx_ = false;
            db_->log_.emplace_back("ROLLBACK");
            return result_ok();
        }

        [[nodiscard]] bool in_transaction() const override { return in_tx_; }

    private:
        memory_db* db_;
        bool in_tx_ = false;
    };

    std::unordered_map<std::string, query_handler> queries_;
    std::unordered_map<std::string, exec_handler> execs_;
    std::vector<std::string> log_;
};

}  // namespace utils::sql

# sql（关系库门面）

薄 SQL + 事务门面：统一 `query` / `exec` / 事务，不绑 SOCI/libpqxx，不做 ORM。  
KV（Redis 等）不要塞进本门面。网络请直接用 Asio/Beast。

| 项 | 说明 |
|----|------|
| 头文件 | [`adapter/sql/sql.h`](../adapter/sql/sql.h) |
| 命名空间 | `utils::sql` |
| 伞头 | 已由 `utils/utils.h` 重导出 |
| 示例 | `demo/sql/demo.cpp`（`demo_sql`） |
| 错误 | [`result`](./result.md) |

---

## 目录

1. [快速入门](#1-快速入门)
2. [类型别名](#2-类型别名)
3. [connection](#3-connection)
4. [database](#4-database)
5. [memory_db](#5-memory_db)
6. [扩展真库](#6-扩展真库)
7. [注意事项](#7-注意事项)

---

## 1. 快速入门

```cpp
#include "adapter/sql/sql.h"
using namespace utils;

auto db = std::make_shared<sql::memory_db>();
db->on_query("SELECT 1", sql::result_set{{"n"}, {{std::string{"1"}}}});

auto conn = db->open();
if (!conn) { /* conn.error() */ }

auto rows = conn.value()->query("SELECT 1");
if (rows) {
    for (auto& row : rows->rows) { /* ... */ }
}

conn.value()->begin();
conn.value()->exec("UPDATE t SET x=1");
conn.value()->commit();
```

---

## 2. 类型别名

```cpp
namespace utils::sql {
using value = std::optional<std::string>;
using row = std::vector<value>;
using params = std::vector<value>;

struct result_set {
    std::vector<std::string> columns;
    std::vector<row> rows;
};
}
```

单元格用 `optional<string>` 表示可空。

---

## 3. connection

```cpp
class connection {
public:
    virtual ~connection() = default;

    virtual result<void> exec(std::string_view sql,
                              const params& p = {}) = 0;
    virtual result<result_set> query(std::string_view sql,
                                     const params& p = {}) = 0;
    virtual result<void> begin() = 0;
    virtual result<void> commit() = 0;
    virtual result<void> rollback() = 0;
    virtual bool in_transaction() const = 0;
};
```

| API | 说明 |
|-----|------|
| `exec` | 执行非查询 SQL |
| `query` | 查询，返回列名 + 行 |
| `begin` / `commit` / `rollback` | 事务 |
| `in_transaction` | 是否处于事务中 |

---

## 4. database

```cpp
class database {
public:
    virtual ~database() = default;
    virtual result<std::shared_ptr<connection>> open() = 0;
};
```

业务依赖 `database` / `connection` 抽象，便于注入假实现。

---

## 5. memory_db

测试用内存实现：按 **SQL 全文** 注册 handler。

| API | 说明 |
|-----|------|
| `void on_query(string sql, result_set canned)` | 固定结果 |
| `void on_query(string sql, query_handler)` | `result_set(const params&)` |
| `void on_exec(string sql, exec_handler)` | `result<void>(const params&)` |
| `open()` | 得到 connection |
| `const vector<string>& log() const` | 已执行 SQL / BEGIN/COMMIT/ROLLBACK 记录 |

```cpp
db->on_query(
    "SELECT id FROM t WHERE name=?",
    sql::result_set{{"id"}, {{std::string{"7"}}}});

auto q = conn.value()->query(
    "SELECT id FROM t WHERE name=?",
    {std::string{"alice"}});
```

行为约定：

- 未注册的 `query` → 空 `result_set`
- 未注册的 `exec` → `result_ok()`
- 事务只记标志，**不回滚数据**
- 嵌套 `begin` / 无事务 `commit`·`rollback` → `result_err(invalid_argument)`

---

## 6. 扩展真库

实现 `sql::connection` + `sql::database`，在适配器内转换第三方类型，**不要**把 SOCI/libpqxx 类型放进业务头文件。

```cpp
class pqxx_database : public sql::database {
    result<std::shared_ptr<connection>> open() override;
};
```

---

## 7. 注意事项

1. 本门面只覆盖关系库访问的薄层；不做查询构建器 / ORM。
2. `memory_db` 用 SQL 字符串精确匹配，测试里保持 SQL 文本一致。
3. 失败路径统一走 `result`，与仓库其它模块一致。

# SQL 门面

`facade/sql/sql.h` 定义 `database` 与 `connection` 的窄接口，不绑定驱动，
也不实现 ORM。内置 `memory_db` 按 SQL 全文匹配 handler，适合单元测试。

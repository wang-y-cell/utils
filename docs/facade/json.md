# JSON 门面

`facade/json/json.h` 提供不依赖第三方库的中立 DOM、解析和序列化接口。
默认 `simple_backend` 支持标准 JSON 的对象、数组、标量、Unicode 转义、
小数和指数，严格拒绝注释及尾逗号。

```cpp
auto document = utils::json::parse(R"({"db":{"host":"localhost"}})");
auto host = document->get("db.host");
document->set("db.pool", utils::json::value(std::int64_t{4}));
auto text = utils::json::stringify(*document, true);
```

路径仅支持以 `.` 分隔的对象键。解析、类型转换、路径和非法操作失败均通过
`utils::result` 返回。

## 替换后端

第三方适配器继承 `utils::json::backend`，实现 `parse` 和 `stringify`，
并在第三方 DOM 与 `json::value` 之间转换：

```cpp
utils::json::set_backend(std::make_shared<my_json_backend>());
auto document = utils::json::parse(input);
utils::json::reset_backend();
```

后端指针切换是线程安全的。每次解析或序列化只增加一次短锁和虚函数调用；
主要额外成本来自第三方 DOM 与中立 DOM 之间的转换。

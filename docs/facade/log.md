# Log 门面

`facade/log/log.h` 提供级别过滤与可替换的 `backend`。业务代码只依赖
`trace/debug/info/warn/error`，测试可切换到 `null_backend` 或自定义捕获后端。

# 扩展门面规划

当前公开主题为 `concurrency`、`reliability`、`memory` 和 `facade`。
`facade` 已包含 log、sql 与 json。

仅在以下条件同时成立时新增门面：

1. 后端可能替换，或单元测试明确需要假实现。
2. 第三方 API 会把大量类型和配置泄漏进业务层。
3. 少量核心方法可覆盖主要用法。

未来可按需评估 metrics 或 kv_store。网络库、完整 ORM、GoF 模式类库和通用
allocator 不在本项目范围内。新门面放入 `facade/`，失败路径继续使用
`reliability/result/expected.h`，公开 API 不暴露第三方类型。

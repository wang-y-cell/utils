# Memory

`memory/memory.h` 导出两个线程安全固定块组件：

- `memory_pool` 只管理原始固定大小块，不调用构造或析构。
- `object_pool<T>` 在 `memory_pool` 上提供 `create/destroy` 与 RAII `acquire`。

对象池必须比由它创建的所有对象活得更久。池析构前也必须归还所有活跃块。

```cpp
utils::object_pool<std::string> pool(32);
auto value = pool.acquire("hello");
```

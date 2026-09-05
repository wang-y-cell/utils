#pragma once

#include "memory/memory_pool.h"

#include <cstddef>
#include <memory>
#include <source_location>
#include <type_traits>
#include <utility>

namespace utils {

/**
 * 固定类型对象池（底层 memory_pool，默认无锁）。
 *
 * object_pool 必须比由 create/acquire 创建的所有对象活得更久。
 * 泄漏记录在底层 memory_pool（UTILS_POOL_LEAK_CHECK）。
 */
template <class T>
class object_pool {
public:
    class deleter {
    public:
        deleter() noexcept = default;
        explicit deleter(object_pool* pool) noexcept : pool_(pool) {}

        void operator()(T* p) const
            noexcept(std::is_nothrow_destructible_v<T>) {
            if (pool_) pool_->destroy(p);
        }

    private:
        object_pool* pool_ = nullptr;
    };

    using pointer = std::unique_ptr<T, deleter>;

    explicit object_pool(std::size_t objects_per_chunk = 64)
        : storage_(sizeof(T), objects_per_chunk, alignof(T)) {}

    object_pool(const object_pool&) = delete;
    object_pool& operator=(const object_pool&) = delete;
    object_pool(object_pool&&) = delete;
    object_pool& operator=(object_pool&&) = delete;

    template <class... Args>
    [[nodiscard]] T* create(Args&&... args) {
#if UTILS_POOL_LEAK_CHECK >= 2
        return create_at(std::source_location::current(),
                         std::forward<Args>(args)...);
#else
        void* storage = storage_.allocate();
        try {
            return std::construct_at(static_cast<T*>(storage),
                                     std::forward<Args>(args)...);
        } catch (...) {
            storage_.deallocate(storage);
            throw;
        }
#endif
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    template <class... Args>
    [[nodiscard]] T* create_at(std::source_location loc, Args&&... args) {
        void* storage = storage_.allocate(loc);
        try {
            return std::construct_at(static_cast<T*>(storage),
                                     std::forward<Args>(args)...);
        } catch (...) {
            storage_.deallocate(storage);
            throw;
        }
    }
#endif

    void destroy(T* p) noexcept(std::is_nothrow_destructible_v<T>) {
        if (!p) return;

        if constexpr (std::is_nothrow_destructible_v<T>) {
            std::destroy_at(p);
            storage_.deallocate(p);
        } else {
            try {
                std::destroy_at(p);
            } catch (...) {
                storage_.deallocate(p);
                throw;
            }
            storage_.deallocate(p);
        }
    }

    /// 创建一个对象,并返回一个智能指针
    template <class... Args>
    [[nodiscard]] pointer acquire(Args&&... args) {
        return pointer(create(std::forward<Args>(args)...), deleter{this});
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    /// 创建一个对象,并返回一个智能指针,指定分配位置
    template <class... Args>
    [[nodiscard]] pointer acquire_at(std::source_location loc, Args&&... args) {
        return pointer(create_at(loc, std::forward<Args>(args)...),
                       deleter{this});
    }
#endif

    /// 当前池中总共的内存块数
    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.capacity();
    }

    /// 当前池中可用的内存块数
    [[nodiscard]] std::size_t available() const noexcept {
        return storage_.available();
    }

    /// 当前池中正在使用的内存块数
    [[nodiscard]] std::size_t in_use() const noexcept {
        return storage_.in_use();
    }

    /// 获取底层内存池
    [[nodiscard]] memory_pool& storage() noexcept { return storage_; }
    /// 获取底层内存池
    [[nodiscard]] const memory_pool& storage() const noexcept {
        return storage_;
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    /// 将泄漏的信息写入标准错误输出的辅助函数
    void dump_leaks() const { storage_.dump_leaks(); }
#endif

private:
    /// 底层内存池
    memory_pool storage_;
};

}  // namespace utils

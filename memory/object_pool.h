#pragma once

#include "memory/memory_pool.h"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace utils {

/**
 * 固定类型对象池。
 *
 * object_pool 必须比由 create/acquire 创建的所有对象活得更久。
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
        void* storage = storage_.allocate();
        try {
            return std::construct_at(static_cast<T*>(storage),
                                     std::forward<Args>(args)...);
        } catch (...) {
            storage_.deallocate(storage);
            throw;
        }
    }

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

    template <class... Args>
    [[nodiscard]] pointer acquire(Args&&... args) {
        return pointer(create(std::forward<Args>(args)...), deleter{this});
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.capacity();
    }

    [[nodiscard]] std::size_t available() const noexcept {
        return storage_.available();
    }

    [[nodiscard]] std::size_t in_use() const noexcept {
        return storage_.in_use();
    }

private:
    memory_pool storage_;
};

}  // namespace utils

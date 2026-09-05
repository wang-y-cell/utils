#pragma once

/**
 * typed_alloc — 按类型 T 适配 memory_allocator（类似 SGI simple_alloc）。
 * 调用方只传「几个 T」，不必记字节数。
 */

#include "memory/memory_allocator.h"

#include <cstddef>
#if UTILS_POOL_LEAK_CHECK >= 2
#include <source_location>
#endif

namespace utils {

template <class T>
class typed_alloc {
public:
    using value_type = T;

    explicit typed_alloc(memory_allocator& alloc) noexcept : alloc_(&alloc) {}

    [[nodiscard]] T* allocate(std::size_t n = 1
#if UTILS_POOL_LEAK_CHECK >= 2
                              ,
                              std::source_location loc =
                                  std::source_location::current()
#endif
    ) {
        if (n == 0) {
            return nullptr;
        }
        return static_cast<T*>(alloc_->allocate(n * sizeof(T)
#if UTILS_POOL_LEAK_CHECK >= 2
                                                    ,
                                                loc
#endif
                                                ));
    }

    void deallocate(T* p, std::size_t n = 1) noexcept {
        if (!p || n == 0) {
            return;
        }
        alloc_->deallocate(p, n * sizeof(T));
    }

    [[nodiscard]] memory_allocator& allocator() noexcept { return *alloc_; }
    [[nodiscard]] const memory_allocator& allocator() const noexcept {
        return *alloc_;
    }

private:
    memory_allocator* alloc_;
};

}  // namespace utils

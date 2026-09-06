#pragma once

/**
 * typed_alloc — 按类型适配字节分配器（类似 SGI simple_alloc）。
 *
 * 无需手动配置底层池；默认使用每线程一份 thread_local Alloc（契合无锁设计）：
 *
 *   int* p = utils::typed_alloc<int>::allocate(8);
 *   utils::typed_alloc<int>::deallocate(p, 8);
 *
 * 若要自定义/观察底层：
 *   auto& raw = utils::typed_alloc<int>::allocator();
 *
 * Alloc 需提供 allocate(bytes) / deallocate_unchecked(p, bytes)。
 */

#include "memory/memory_allocator.h"

#include <cstddef>
#if UTILS_POOL_LEAK_CHECK >= 2
#include <source_location>
#endif

namespace utils {

template <class T, class Alloc = memory_allocator>
class typed_alloc {
public:
    using value_type = T;
    using allocator_type = Alloc;

    typed_alloc() = delete;

    /** 本线程的底层 Alloc */
    [[nodiscard]] static Alloc& allocator() {
        thread_local Alloc alloc;
        return alloc;
    }

    [[nodiscard]] static T* allocate(std::size_t n = 1
#if UTILS_POOL_LEAK_CHECK >= 2
                                     ,
                                     std::source_location loc =
                                         std::source_location::current()
#endif
    ) {
        if (n == 0) {
            return nullptr;
        }
        return static_cast<T*>(allocator().allocate(n * sizeof(T)
#if UTILS_POOL_LEAK_CHECK >= 2
                                                        ,
                                                    loc
#endif
                                                    ));
    }

    static void deallocate(T* p, std::size_t n = 1) noexcept {
        if (!p || n == 0) {
            return;
        }
        allocator().deallocate_unchecked(p, n * sizeof(T));
    }
};

}  // namespace utils

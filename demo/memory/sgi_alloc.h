#pragma once

/**
 * 用户提供的 SGI 风格二级分配器（静态 freelist，非线程安全）。
 * 用于与 ::operator new / utils::memory_allocator 对比。
 */

#include <cstddef>
#include <cstdlib>
#include <new>

namespace msl {

#define __THROW_BAD_ALLOC throw std::bad_alloc()

class malloc_alloc_template {
private:
    static void* oom_malloc(size_t);
    static void* oom_realloc(void* p, size_t new_sz);
    static void (*__malloc_alloc_oom_handler)();

public:
    static void* allocate(size_t n) {
        void* p = malloc(n);
        if (!p) p = oom_malloc(n);
        return p;
    }

    static void deallocate(void* p, size_t) { free(p); }

    static void* reallocate(void* p, size_t, size_t new_sz) {
        void* r = realloc(p, new_sz);
        if (!r) r = oom_realloc(p, new_sz);
        return r;
    }

    static void (*set_malloc_handler(void (*f)()))() {
        void (*old)() = __malloc_alloc_oom_handler;
        __malloc_alloc_oom_handler = f;
        return old;
    }
};

inline void (*malloc_alloc_template::__malloc_alloc_oom_handler)() = nullptr;

inline void* malloc_alloc_template::oom_malloc(size_t n) {
    for (;;) {
        void (*handler)() = __malloc_alloc_oom_handler;
        if (!handler) __THROW_BAD_ALLOC;
        (*handler)();
        void* ret = malloc(n);
        if (ret) return ret;
    }
}

inline void* malloc_alloc_template::oom_realloc(void* p, size_t new_sz) {
    for (;;) {
        void (*handler)() = __malloc_alloc_oom_handler;
        if (!handler) __THROW_BAD_ALLOC;
        (*handler)();
        void* ret = realloc(p, new_sz);
        if (ret) return ret;
    }
}

using malloc_alloc = malloc_alloc_template;

enum { ALIGN = 8 };
enum { MAX_BYTES = 128 };
enum { NFREELISTS = MAX_BYTES / ALIGN };

class default_alloc_template {
private:
    union obj {
        union obj* free_list_link;
        char client_data[1];
    };

    static obj* free_list[NFREELISTS];
    static char* start_free;
    static char* end_free;
    static size_t heap_size;

    static size_t ROUND_UP(size_t bytes) {
        return (bytes + ALIGN - 1) & ~(ALIGN - 1);
    }
    static size_t FREELIST_INDEX(size_t bytes) {
        return (bytes + ALIGN - 1) / ALIGN - 1;
    }
    static void* refill(size_t n);
    static char* chunk_alloc(size_t size, int& nobjs);

public:
    static void* allocate(std::size_t n) {
        if (n > MAX_BYTES) return malloc_alloc::allocate(n);
        size_t index = FREELIST_INDEX(n);
        obj** my_list = free_list + index;
        obj* result = *my_list;
        if (result) {
            *my_list = result->free_list_link;
            return result;
        }
        return refill(ROUND_UP(n));
    }

    static void deallocate(void* p, size_t n) {
        if (!p || n == 0) return;
        if (n > (size_t)MAX_BYTES) {
            malloc_alloc::deallocate(p, n);
            return;
        }
        obj* q = (obj*)p;
        obj** my_free_list = free_list + FREELIST_INDEX(n);
        q->free_list_link = *my_free_list;
        *my_free_list = q;
    }

    static void* reallocate(void* p, size_t old_sz, size_t new_sz) {
        if (old_sz > MAX_BYTES && new_sz > MAX_BYTES)
            return malloc_alloc::reallocate(p, old_sz, new_sz);
        if (ROUND_UP(old_sz) == ROUND_UP(new_sz)) return p;
        void* result = allocate(new_sz);
        size_t copy_sz = old_sz < new_sz ? old_sz : new_sz;
        auto* r = (unsigned char*)result;
        auto* s = (unsigned char*)p;
        for (size_t i = 0; i < copy_sz; ++i) r[i] = s[i];
        deallocate(p, old_sz);
        return result;
    }
};

inline typename default_alloc_template::obj*
    default_alloc_template::free_list[NFREELISTS] = {nullptr};

inline char* default_alloc_template::start_free = nullptr;
inline char* default_alloc_template::end_free = nullptr;
inline size_t default_alloc_template::heap_size = 0;

inline void* default_alloc_template::refill(size_t n) {
    int nobjs = 20;
    char* chunk = chunk_alloc(n, nobjs);
    if (nobjs == 1) return (void*)chunk;
    size_t index = FREELIST_INDEX(n);
    obj** my_list = free_list + index;
    char* cur = chunk + n;
    *my_list = (obj*)cur;
    obj* current = (obj*)cur;
    for (int i = 1; i < nobjs - 1; ++i) {
        char* next = cur + n;
        current->free_list_link = (obj*)next;
        current = (obj*)next;
        cur = next;
    }
    current->free_list_link = nullptr;
    return (void*)chunk;
}

inline char* default_alloc_template::chunk_alloc(size_t size, int& nobjs) {
    size_t total_bytes = size * nobjs;
    size_t bytes_left = end_free - start_free;
    char* result;
    if (bytes_left >= total_bytes) {
        result = start_free;
        start_free += total_bytes;
        return result;
    }
    if (bytes_left >= size) {
        nobjs = (int)(bytes_left / size);
        total_bytes = size * nobjs;
        result = start_free;
        start_free += total_bytes;
        return result;
    }
    if (bytes_left > 0) {
        size_t index = FREELIST_INDEX(bytes_left);
        obj** my_list = free_list + index;
        ((obj*)start_free)->free_list_link = *my_list;
        *my_list = (obj*)start_free;
    }
    size_t bytes_to_get = 2 * total_bytes + ROUND_UP(heap_size >> 4);
    start_free = (char*)malloc(bytes_to_get);
    if (start_free == 0) {
        for (int i = (int)size; i <= MAX_BYTES; i += ALIGN) {
            obj** me_free_list = free_list + FREELIST_INDEX(i);
            obj* p = *me_free_list;
            if (p) {
                *me_free_list = p->free_list_link;
                start_free = (char*)p;
                end_free = start_free + i;
                return chunk_alloc(size, nobjs);
            }
        }
        end_free = 0;
        start_free = (char*)malloc_alloc::allocate(bytes_to_get);
    }
    heap_size += bytes_to_get;
    end_free = start_free + bytes_to_get;
    return chunk_alloc(size, nobjs);
}

using default_alloc = default_alloc_template;
using alloc = default_alloc;

}  // namespace msl

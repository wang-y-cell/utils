#pragma once

/**
 * 固定块内存池（默认无锁；线程安全由调用方保证）。
 *
 * 热路径目标：档 0 仅 freelist 弹/压，接近 boost::pool。
 * 析构前必须归还全部块；不负责对象构造/析构（见 object_pool<T>）。
 *
 * 泄漏档 UTILS_POOL_LEAK_CHECK（默认 0）：
 *   0 — 热路径不维护 available 计数；stats 查询可 O(n) 遍历 freelist
 *   1 — O(1) 计数；析构未归还告警
 *   2 — ptr→source_location + dump
 */

#ifndef UTILS_POOL_LEAK_CHECK
#define UTILS_POOL_LEAK_CHECK 0
#endif

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <vector>

#if UTILS_POOL_LEAK_CHECK >= 2
#include <source_location>
#include <unordered_map>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define UTILS_POOL_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define UTILS_POOL_ALWAYS_INLINE __forceinline
#else
#define UTILS_POOL_ALWAYS_INLINE inline
#endif

namespace utils {

class memory_pool {
public:
    explicit memory_pool(
        std::size_t block_size, std::size_t blocks_per_chunk = 32,
        std::size_t alignment = alignof(std::max_align_t))
        : blocks_per_chunk_(blocks_per_chunk),
          next_size_(blocks_per_chunk),
          alignment_(std::max(alignment, alignof(void*))) {
        if (block_size == 0) {
            throw std::invalid_argument("memory_pool: block_size must be > 0");
        }
        if (blocks_per_chunk_ == 0) {
            throw std::invalid_argument(
                "memory_pool: blocks_per_chunk must be > 0");
        }
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            throw std::invalid_argument(
                "memory_pool: alignment must be a power of two");
        }

        const std::size_t minimum = std::max(block_size, sizeof(free_node));
        block_size_ =
            (minimum + alignment_ - 1) &
            ~(static_cast<std::size_t>(alignment_ - 1));
    }

    ~memory_pool() {
#if UTILS_POOL_LEAK_CHECK >= 1
        const std::size_t outstanding = capacity_ - available_;
        if (outstanding != 0) {
            std::fprintf(stderr,
                         "memory_pool: destroy with %zu block(s) still in "
                         "use (block_size=%zu)\n",
                         outstanding, block_size_);
#if UTILS_POOL_LEAK_CHECK >= 2
            dump_leaks_to(stderr);
            report_leaks_to_file();
#endif
        }
#endif
        for (void* chunk : chunks_) {
            ::operator delete(chunk, std::align_val_t(alignment_));
        }
    }

    memory_pool(const memory_pool&) = delete;
    memory_pool& operator=(const memory_pool&) = delete;
    memory_pool(memory_pool&&) = delete;
    memory_pool& operator=(memory_pool&&) = delete;

    /** 热路径：空 freelist 时扩容；档 0 不维护 available_ */
    [[nodiscard]] UTILS_POOL_ALWAYS_INLINE void* allocate(
#if UTILS_POOL_LEAK_CHECK >= 2
        std::source_location loc = std::source_location::current()
#endif
    ) {
        if (!free_) [[unlikely]] {
            grow();
        }
        free_node* node = free_;
        free_ = free_->next;
#if UTILS_POOL_LEAK_CHECK >= 1
        --available_;
#endif
#if UTILS_POOL_LEAK_CHECK >= 2
        sites_[node] = loc;
#endif
        return node;
    }

    /** 允许 p==nullptr（空操作） */
    UTILS_POOL_ALWAYS_INLINE void deallocate(void* p) noexcept {
        if (!p) [[unlikely]] {
            return;
        }
        deallocate_unchecked(p);
    }

    /** 调用方保证 p 非空且来自本池；少一次分支 */
    UTILS_POOL_ALWAYS_INLINE void deallocate_unchecked(void* p) noexcept {
#if UTILS_POOL_LEAK_CHECK >= 2
        sites_.erase(p);
#endif
        auto* node = static_cast<free_node*>(p);
        node->next = free_;
        free_ = node;
#if UTILS_POOL_LEAK_CHECK >= 1
        ++available_;
#endif
    }

    [[nodiscard]] std::size_t block_size() const noexcept {
        return block_size_;
    }
    [[nodiscard]] std::size_t alignment() const noexcept {
        return alignment_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] std::size_t available() const noexcept {
#if UTILS_POOL_LEAK_CHECK >= 1
        return available_;
#else
        std::size_t n = 0;
        for (free_node* p = free_; p; p = p->next) {
            ++n;
        }
        return n;
#endif
    }

    [[nodiscard]] std::size_t in_use() const noexcept {
        return capacity_ - available();
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    void dump_leaks() const { dump_leaks_to(stderr); }
#endif

private:
    struct free_node {
        free_node* next;
    };

    void grow() {
        const std::size_t n = next_size_;
        const std::size_t bytes = block_size_ * n;
        void* chunk = ::operator new(bytes, std::align_val_t(alignment_));
        try {
            chunks_.push_back(chunk);
        } catch (...) {
            ::operator delete(chunk, std::align_val_t(alignment_));
            throw;
        }

        auto* bytes_begin = static_cast<std::byte*>(chunk);
        // 顺序挂链，略利于预取（与 Boost segregated storage 类似）
        free_node* head = free_;
        for (std::size_t i = n; i-- > 0;) {
            auto* node =
                reinterpret_cast<free_node*>(bytes_begin + i * block_size_);
            node->next = head;
            head = node;
        }
        free_ = head;

        capacity_ += n;
#if UTILS_POOL_LEAK_CHECK >= 1
        available_ += n;
#endif
        // 下次扩容加倍（Boost next_size 风格），有上限避免单次过大
        if (next_size_ < (std::size_t{1} << 20) / block_size_) {
            next_size_ *= 2;
        }
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    void dump_leaks_to(FILE* out) const {
        std::fprintf(out, "memory_pool outstanding=%zu\n", sites_.size());
        for (const auto& [ptr, loc] : sites_) {
            std::fprintf(out, "  ptr=%p  %s:%u  %s\n", ptr, loc.file_name(),
                         loc.line(), loc.function_name());
        }
    }

    void report_leaks_to_file() const {
        const char* path = std::getenv("UTILS_POOL_LEAK_FILE");
        if (!path || !*path || sites_.empty()) {
            return;
        }
        FILE* f = std::fopen(path, "a");
        if (!f) {
            return;
        }
        dump_leaks_to(f);
        std::fclose(f);
    }

    std::unordered_map<void*, std::source_location> sites_;
#endif

    std::size_t block_size_ = 0;
    std::size_t blocks_per_chunk_;
    std::size_t next_size_;
    std::size_t alignment_;
    free_node* free_ = nullptr;
    std::vector<void*> chunks_;
    std::size_t capacity_ = 0;
#if UTILS_POOL_LEAK_CHECK >= 1
    std::size_t available_ = 0;
#endif
};

}  // namespace utils

#undef UTILS_POOL_ALWAYS_INLINE

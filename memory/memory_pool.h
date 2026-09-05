#pragma once

/**
 * 固定块内存池（默认无锁，追求速度；线程安全由调用方保证）。
 *
 * 按 chunk 扩容；不负责对象构造/析构（见 object_pool<T>）。
 * 析构前必须归还全部块。
 *
 * 泄漏档（编译期 UTILS_POOL_LEAK_CHECK，默认 0）：
 *   0 — 热路径零额外记录
 *   1 — 析构时若 in_use()!=0 告警（沿用 capacity/available）
 *   2 — ptr→source_location，可 dump_leaks()
 */

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <source_location>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifndef UTILS_POOL_LEAK_CHECK
#define UTILS_POOL_LEAK_CHECK 0
#endif

namespace utils {

class memory_pool {
public:
    explicit memory_pool(
        std::size_t block_size, std::size_t blocks_per_chunk = 64,
        std::size_t alignment = alignof(std::max_align_t))
        : blocks_per_chunk_(blocks_per_chunk),
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

    [[nodiscard]] void* allocate(
#if UTILS_POOL_LEAK_CHECK >= 2
        std::source_location loc = std::source_location::current()
#endif
    ) {
        if (!free_) {
            grow();
        }
        free_node* node = free_;
        free_ = free_->next;
        --available_;
#if UTILS_POOL_LEAK_CHECK >= 2
        sites_[node] = loc;
#endif
        return node;
    }

    void deallocate(void* p) noexcept {
        if (!p) {
            return;
        }
#if UTILS_POOL_LEAK_CHECK >= 2
        sites_.erase(p);
#endif
        auto* node = static_cast<free_node*>(p);
        node->next = free_;
        free_ = node;
        ++available_;
    }

    [[nodiscard]] std::size_t block_size() const noexcept {
        return block_size_;
    }
    [[nodiscard]] std::size_t alignment() const noexcept {
        return alignment_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t available() const noexcept {
        return available_;
    }
    [[nodiscard]] std::size_t in_use() const noexcept {
        return capacity_ - available_;
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    void dump_leaks() const { dump_leaks_to(stderr); }
#endif

private:
    struct free_node {
        free_node* next;
    };

    void grow() {
        const std::size_t bytes = block_size_ * blocks_per_chunk_;
        void* chunk = ::operator new(bytes, std::align_val_t(alignment_));
        try {
            chunks_.push_back(chunk);
        } catch (...) {
            ::operator delete(chunk, std::align_val_t(alignment_));
            throw;
        }

        auto* bytes_begin = static_cast<std::byte*>(chunk);
        for (std::size_t i = 0; i < blocks_per_chunk_; ++i) {
            auto* node =
                reinterpret_cast<free_node*>(bytes_begin + i * block_size_);
            node->next = free_;
            free_ = node;
        }
        capacity_ += blocks_per_chunk_;
        available_ += blocks_per_chunk_;
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
    std::size_t alignment_;
    free_node* free_ = nullptr;
    std::vector<void*> chunks_;
    std::size_t capacity_ = 0;
    std::size_t available_ = 0;
};

}  // namespace utils

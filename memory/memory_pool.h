#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

namespace utils {

/**
 * 线程安全的固定块内存池。
 *
 * 按 chunk 扩容，不负责调用对象构造/析构；类型对象请使用 object_pool<T>。
 * pool 析构前，调用者必须归还所有仍在使用的块。
 */
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
            (minimum + alignment_ - 1) & ~(static_cast<std::size_t>(alignment_ - 1));
    }

    ~memory_pool() {
        for (void* chunk : chunks_) {
            ::operator delete(chunk, std::align_val_t(alignment_));
        }
    }

    memory_pool(const memory_pool&) = delete;
    memory_pool& operator=(const memory_pool&) = delete;
    memory_pool(memory_pool&&) = delete;
    memory_pool& operator=(memory_pool&&) = delete;

    [[nodiscard]] void* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!free_) {
            grow_unlocked();
        }

        free_node* node = free_;
        free_ = free_->next;
        --available_;
        return node;
    }

    void deallocate(void* p) noexcept {
        if (!p) return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto* node = static_cast<free_node*>(p);
        node->next = free_;
        free_ = node;
        ++available_;
    }

    [[nodiscard]] std::size_t block_size() const noexcept {
        return block_size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    [[nodiscard]] std::size_t available() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return available_;
    }

    [[nodiscard]] std::size_t in_use() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_ - available_;
    }

private:
    struct free_node {
        free_node* next;
    };

    void grow_unlocked() {
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

    std::size_t block_size_ = 0;
    std::size_t blocks_per_chunk_;
    std::size_t alignment_;
    mutable std::mutex mutex_;
    free_node* free_ = nullptr;
    std::vector<void*> chunks_;
    std::size_t capacity_ = 0;
    std::size_t available_ = 0;
};

}  // namespace utils

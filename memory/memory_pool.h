#pragma once

/**
 * 线程安全的固定块内存池。
 *
 * 按 chunk 扩容，不负责对象构造/析构；类型对象请使用 object_pool<T>。
 * pool 析构前，调用者必须归还所有仍在使用的块。
 *
 * 泄漏档位（编译期 UTILS_POOL_LEAK_CHECK，默认 0）：
 *   0 — 无额外记录
 *   1 — 析构时若 in_use()!=0 告警（计数沿用 capacity-available）
 *   2 — 记录 ptr→source_location，可 dump_leaks()；析构 dump + 可选写文件
 */

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef UTILS_POOL_LEAK_CHECK
#define UTILS_POOL_LEAK_CHECK 0
#endif

namespace utils {

class memory_pool {
public:
    /// @param block_size 块大小
    /// @param blocks_per_chunk 一次向系统要多少块
    /// @param alignment 对齐大小
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
        //align 必须为0 或2的幂
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            throw std::invalid_argument(
                "memory_pool: alignment must be a power of two");
        }

        const std::size_t minimum = std::max(block_size, sizeof(free_node));
        //根据对齐大小,选择需要对齐的整数倍
        block_size_ =
            (minimum + alignment_ - 1) & ~(static_cast<std::size_t>(alignment_ - 1));
    }

    ~memory_pool() {
#if UTILS_POOL_LEAK_CHECK >= 1
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t outstanding = capacity_ - available_;
            if (outstanding != 0) {
                std::fprintf(stderr,
                             "memory_pool: destroy with %zu block(s) still in "
                             "use (block_size=%zu)\n",
                             outstanding, block_size_);
#if UTILS_POOL_LEAK_CHECK >= 2
                dump_leaks_to(stderr);
                report_leaks_to_file_unlocked();
#endif
            }
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
        std::source_location loc = std::source_location::current()) {
        std::lock_guard<std::mutex> lock(mutex_);
        //如果当前free指向空,表示池中没有可用的内存了,需要向系统申请新的大块内存
        if (!free_) {
            grow_unlocked();
        }

        //获取当前free指向的内存,并将其next指向free的next
        free_node* node = free_;
        free_ = free_->next;
        --available_;
#if UTILS_POOL_LEAK_CHECK >= 2
        //记录当前块在什么地方被分配的,用于泄漏检查
        sites_[node] = loc;
#else
        (void)loc; //如果不需要泄漏检查,则忽略loc
#endif
        return node; //返回当前块的内存地址
    }

    void deallocate(void* p) noexcept {
        if (!p) return; //如果p为空,则返回

        std::lock_guard<std::mutex> lock(mutex_);
#if UTILS_POOL_LEAK_CHECK >= 2
        //删除当前块在什么地方被分配的记录,用于泄漏检查
        sites_.erase(p);
#endif
        auto* node = static_cast<free_node*>(p);
        node->next = free_;
        free_ = node;
        ++available_;
    }

    /// 一个块大小
    [[nodiscard]] std::size_t block_size() const noexcept {
        return block_size_;
    }

    /// 对齐大小
    [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }

    /// 当前池中总共的内存块数
    [[nodiscard]] std::size_t capacity() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    /// 当前池中可用的内存块数
    [[nodiscard]] std::size_t available() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return available_;
    }

    /// 当前池中正在使用的内存块数
    [[nodiscard]] std::size_t in_use() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_ - available_;
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    /// 将泄漏的信息写入标准错误输出的辅助函数
    void dump_leaks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        dump_leaks_to(stderr);
    }
#endif

private:
    struct free_node {
        free_node* next;
    };

    /// 当前没有可用内存了, 向系统申请新的内存块的辅助函数
    void grow_unlocked() {
        //获得需要向系统申请的内存大小,作为池
        const std::size_t bytes = block_size_ * blocks_per_chunk_;
        void* chunk = ::operator new(bytes, std::align_val_t(alignment_));

        try {
            chunks_.push_back(chunk);
        } catch (...) {
            ::operator delete(chunk, std::align_val_t(alignment_));
            throw;
        }

        //配置这个大块为一个链表
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
    /// 将泄漏的信息写入(文件/标准错误输出)的辅助函数
    void dump_leaks_to(FILE* out) const {
        std::fprintf(out, "memory_pool outstanding=%zu\n", sites_.size());
        for (const auto& [ptr, loc] : sites_) {
            std::fprintf(out, "  ptr=%p  %s:%u  %s\n", ptr, loc.file_name(),
                         loc.line(), loc.function_name());
        }
    }

    /// 将泄漏的信息写入文件的辅助函数
    /// 会读取环境变量UTILS_POOL_LEAK_FILE，如果存在则写入文件,没有就返回
    void report_leaks_to_file_unlocked() const {
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

    /// 一个块大小
    std::size_t block_size_ = 0;
    /// 一次向系统要多少块
    std::size_t blocks_per_chunk_;
    /// 对齐大小
    std::size_t alignment_;
    /// 互斥锁
    mutable std::mutex mutex_;
    ///free指向的是空闲没有被使用的内存的头部, 注意如果池没有可用的内存,free会指向尾,向系统申请新的内存之后free会指向申请的内存的头部位置,不是整个链表的头部
    free_node* free_ = nullptr;
    std::vector<void*> chunks_;
    /// 当前池中总共的内存块数
    std::size_t capacity_ = 0;
    /// 当前池中可用的内存块数
    std::size_t available_ = 0;
};

}  // namespace utils

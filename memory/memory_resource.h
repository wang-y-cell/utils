#pragma once

/**
 * 多池通用接口：按字节分配的后端契约（可选适配层）。
 * 热路径请直接使用 memory_pool / memory_allocator（无锁）。
 */

#include "memory/memory_pool.h"

#include <cstddef>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#ifndef UTILS_POOL_LEAK_CHECK
#define UTILS_POOL_LEAK_CHECK 0
#endif

namespace utils {

enum class pool_kind {
    auto_select,
    fixed,
    size_class,
    large,
    raw_new,
};

class memory_resource {
public:
    virtual ~memory_resource() = default;

    memory_resource(const memory_resource&) = delete;
    memory_resource& operator=(const memory_resource&) = delete;
    memory_resource(memory_resource&&) = delete;
    memory_resource& operator=(memory_resource&&) = delete;

    [[nodiscard]] void* allocate(
        std::size_t bytes,
        std::size_t alignment = alignof(std::max_align_t),
        std::source_location loc = std::source_location::current()) {
        if (bytes == 0) {
            bytes = 1;
        }
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            throw std::invalid_argument(
                "memory_resource: alignment must be a power of two");
        }
#if UTILS_POOL_LEAK_CHECK >= 2
        return do_allocate(bytes, alignment, loc);
#else
        (void)loc;
        return do_allocate(bytes, alignment);
#endif
    }

    void deallocate(void* p, std::size_t bytes,
                    std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (!p) {
            return;
        }
        do_deallocate(p, bytes, alignment);
    }

    [[nodiscard]] virtual pool_kind kind() const noexcept = 0;
    [[nodiscard]] virtual std::size_t in_use_bytes() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept {
        return "memory_resource";
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    virtual void dump_leaks() const {}
#endif

protected:
    memory_resource() = default;

#if UTILS_POOL_LEAK_CHECK >= 2
    virtual void* do_allocate(std::size_t bytes, std::size_t alignment,
                              std::source_location loc) = 0;
#else
    virtual void* do_allocate(std::size_t bytes, std::size_t alignment) = 0;
#endif
    virtual void do_deallocate(void* p, std::size_t bytes,
                               std::size_t alignment) noexcept = 0;
};

template <class P>
concept fixed_block_pool = requires(P& p, const P& cp, void* ptr) {
    { p.allocate() } -> std::same_as<void*>;
    { p.deallocate(ptr) } -> std::same_as<void>;
    { cp.block_size() } -> std::convertible_to<std::size_t>;
    { cp.in_use() } -> std::convertible_to<std::size_t>;
};

template <fixed_block_pool Pool>
class fixed_block_resource final : public memory_resource {
public:
    explicit fixed_block_resource(Pool& pool,
                                  std::string_view name = "fixed_block") noexcept
        : pool_(&pool), name_(name) {}

    [[nodiscard]] pool_kind kind() const noexcept override {
        return pool_kind::fixed;
    }

    [[nodiscard]] std::size_t in_use_bytes() const noexcept override {
        return pool_->in_use() * pool_->block_size();
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }

    [[nodiscard]] Pool* target() const noexcept { return pool_; }

protected:
#if UTILS_POOL_LEAK_CHECK >= 2
    void* do_allocate(std::size_t bytes, std::size_t /*alignment*/,
                      std::source_location loc) override {
        if (bytes > pool_->block_size()) {
            throw std::invalid_argument(
                "fixed_block_resource: bytes exceed block_size");
        }
        return pool_->allocate(loc);
    }
#else
    void* do_allocate(std::size_t bytes, std::size_t /*alignment*/) override {
        if (bytes > pool_->block_size()) {
            throw std::invalid_argument(
                "fixed_block_resource: bytes exceed block_size");
        }
        return pool_->allocate();
    }
#endif

#if UTILS_POOL_LEAK_CHECK >= 2
    void dump_leaks() const override { pool_->dump_leaks(); }
#endif

    void do_deallocate(void* p, std::size_t /*bytes*/,
                       std::size_t /*alignment*/) noexcept override {
        pool_->deallocate(p);
    }

private:
    Pool* pool_;
    std::string_view name_;
};

}  // namespace utils

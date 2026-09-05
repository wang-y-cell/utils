#pragma once

/**
 * 多池通用接口：按字节分配的后端契约 + 路由用类型。
 *
 * 新池（size-class / large / raw_new 适配器等）应继承 memory_resource，
 * 或满足 fixed_block_pool concept 后再用适配器挂上门面。
 *
 * 泄漏档位（编译期，无运行时切换）：
 *   UTILS_POOL_LEAK_CHECK=0  无记录（默认）
 *   UTILS_POOL_LEAK_CHECK=1  仅 outstanding 计数语义（实现侧维护）
 *   UTILS_POOL_LEAK_CHECK=2  ptr + source_location，可 dump
 */

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

/** 路由 / 后端种类（门面与具体池共用） */
enum class pool_kind {
    auto_select,  // 仅 hint：由门面按 size 选择
    fixed,        // 固定块（如 memory_pool）
    size_class,   // 小块多档
    large,        // 大块策略
    raw_new,      // 原生 new（仍应经门面记账）
};

/** 手动选池时的提示；默认自动 */
struct alloc_hint {
    pool_kind kind = pool_kind::auto_select;
    /** kind==fixed 时指向已有固定块池；类型擦成 void* 避免循环包含 */
    void* fixed_pool = nullptr;
};

/**
 * 按字节分配的抽象后端（多池可插拔）。
 *
 * 约定：
 * - allocate/deallocate 的 bytes/alignment 在归还时必须一致（或由实现自带 meta）
 * - 不负责对象构造/析构
 * - 析构前调用方应归还全部未释放块（leak 档 >=1 时可在析构检查）
 */
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

    /** 当前未归还字节数（档 1/2 的基础观测；档 0 也可实现为近似账本） */
    [[nodiscard]] virtual std::size_t in_use_bytes() const noexcept = 0;

    /** 人类可读名，便于 dump / 日志 */
    [[nodiscard]] virtual std::string_view name() const noexcept {
        return "memory_resource";
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    /** 打印或写出未归还块及分配点；默认空实现，具体池覆盖 */
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

/**
 * 固定块池概念：现有 memory_pool 及未来同类实现应满足。
 * 门面可用适配器把 fixed_block_pool 包成 memory_resource。
 */
template <class P>
concept fixed_block_pool = requires(P& p, const P& cp, void* ptr) {
    { p.allocate() } -> std::same_as<void*>;
    { p.deallocate(ptr) } -> std::same_as<void>;
    { cp.block_size() } -> std::convertible_to<std::size_t>;
    { cp.in_use() } -> std::convertible_to<std::size_t>;
};

/**
 * 把固定块池适配为 memory_resource。
 * 请求 bytes 不得超过 block_size()；超出由调用方/门面改走其它后端。
 */
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

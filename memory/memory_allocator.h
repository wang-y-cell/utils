#pragma once

/**
 * memory_allocator — 多池门面：可配置二级分段 size-class + large(raw new)，
 * 或手动 hint 指定 fixed / raw_new。
 *
 * 默认预设 balanced：
 *   ≤128      步长 8
 *   129～4096  步长 128
 *   > mid_max  raw ::operator new
 *
 * 也可用 size_class_preset / size_class_config 按业务调整步长。
 */

#include "memory/memory_pool.h"
#include "memory/memory_resource.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <source_location>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace utils {

/** 开箱预设；细调请用 size_class_config */
enum class size_class_preset {
    balanced,     // 小步长 8 / 中步长 128（默认）
    dense_small,  // 小段延伸到 256，小对象更密
    compact,      // 档更少，省池实例
};

struct size_class_config {
    std::size_t small_step = 8;
    std::size_t small_max = 128;
    std::size_t mid_step = 128;
    std::size_t mid_max = 4096;
    /** 0 表示使用 mid_max */
    std::size_t large_threshold = 0;
    std::size_t blocks_per_chunk = 64;
    /** 防止步长过小导致档数爆炸 */
    std::size_t max_classes = 256;

    [[nodiscard]] static size_class_config from_preset(
        size_class_preset preset) {
        size_class_config cfg;
        switch (preset) {
            case size_class_preset::balanced:
                break;
            case size_class_preset::dense_small:
                cfg.small_step = 8;
                cfg.small_max = 256;
                cfg.mid_step = 128;
                cfg.mid_max = 4096;
                break;
            case size_class_preset::compact:
                cfg.small_step = 16;
                cfg.small_max = 128;
                cfg.mid_step = 256;
                // (mid_max - small_max) 须整除 mid_step；4224 = 128 + 16*256
                cfg.mid_max = 4224;
                break;
        }
        return cfg;
    }
};

namespace detail {

inline void validate_size_class_config(const size_class_config& cfg) {
    if (cfg.small_step == 0 || cfg.mid_step == 0) {
        throw std::invalid_argument(
            "size_class_config: step must be > 0");
    }
    if (cfg.small_max == 0 || cfg.mid_max == 0) {
        throw std::invalid_argument("size_class_config: max must be > 0");
    }
    if (cfg.small_max % cfg.small_step != 0) {
        throw std::invalid_argument(
            "size_class_config: small_max must be multiple of small_step");
    }
    if (cfg.mid_max <= cfg.small_max) {
        throw std::invalid_argument(
            "size_class_config: mid_max must be > small_max");
    }
    if ((cfg.mid_max - cfg.small_max) % cfg.mid_step != 0) {
        throw std::invalid_argument(
            "size_class_config: (mid_max - small_max) must be multiple of "
            "mid_step");
    }
    if (cfg.blocks_per_chunk == 0) {
        throw std::invalid_argument(
            "size_class_config: blocks_per_chunk must be > 0");
    }
    if (cfg.max_classes == 0) {
        throw std::invalid_argument(
            "size_class_config: max_classes must be > 0");
    }
}

inline std::vector<std::size_t> build_class_sizes(
    const size_class_config& cfg) {
    validate_size_class_config(cfg);

    std::vector<std::size_t> sizes;
    for (std::size_t s = cfg.small_step; s <= cfg.small_max;
         s += cfg.small_step) {
        sizes.push_back(s);
    }
    for (std::size_t s = cfg.small_max + cfg.mid_step; s <= cfg.mid_max;
         s += cfg.mid_step) {
        sizes.push_back(s);
    }
    if (sizes.empty()) {
        throw std::invalid_argument(
            "size_class_config: produced empty class table");
    }
    if (sizes.size() > cfg.max_classes) {
        throw std::invalid_argument(
            "size_class_config: too many classes; increase steps or "
            "max_classes");
    }
    return sizes;
}

}  // namespace detail

class memory_allocator {
public:
    struct stats {
        std::size_t in_use_bytes = 0;
        std::size_t alloc_count = 0;
        std::size_t size_class_bytes = 0;
        std::size_t raw_new_bytes = 0;
        std::size_t fixed_hint_bytes = 0;
    };

    memory_allocator()
        : memory_allocator(
              size_class_config::from_preset(size_class_preset::balanced)) {}

    explicit memory_allocator(size_class_preset preset)
        : memory_allocator(size_class_config::from_preset(preset)) {}

    explicit memory_allocator(size_class_config cfg)
        : config_(cfg),
          class_sizes_(detail::build_class_sizes(cfg)),
          large_threshold_(cfg.large_threshold == 0 ? class_sizes_.back()
                                                    : cfg.large_threshold),
          blocks_per_chunk_(cfg.blocks_per_chunk) {
        if (large_threshold_ < class_sizes_.front()) {
            throw std::invalid_argument(
                "memory_allocator: large_threshold too small");
        }
        classes_.reserve(class_sizes_.size());
        for (std::size_t sz : class_sizes_) {
            classes_.push_back(std::make_unique<memory_pool>(
                sz, blocks_per_chunk_, alignof(std::max_align_t)));
        }
    }

    /**
     * 兼容旧构造：在 balanced 预设上覆盖 large_threshold / blocks_per_chunk。
     */
    explicit memory_allocator(std::size_t large_threshold,
                              std::size_t blocks_per_chunk = 64)
        : memory_allocator([&] {
              auto cfg = size_class_config::from_preset(
                  size_class_preset::balanced);
              cfg.large_threshold = large_threshold;
              cfg.blocks_per_chunk = blocks_per_chunk;
              return cfg;
          }()) {}

    ~memory_allocator() {
#if UTILS_POOL_LEAK_CHECK >= 1
        std::lock_guard<std::mutex> lock(mutex_);
        if (!live_.empty()) {
            std::fprintf(stderr,
                         "memory_allocator: destroy with %zu allocation(s) "
                         "still live (%zu bytes)\n",
                         live_.size(), in_use_bytes_unlocked());
#if UTILS_POOL_LEAK_CHECK >= 2
            dump_leaks_to_unlocked(stderr);
            const char* path = std::getenv("UTILS_POOL_LEAK_FILE");
            if (path && *path) {
                if (FILE* f = std::fopen(path, "a")) {
                    dump_leaks_to_unlocked(f);
                    std::fclose(f);
                }
            }
#endif
        }
#endif
    }

    memory_allocator(const memory_allocator&) = delete;
    memory_allocator& operator=(const memory_allocator&) = delete;

    [[nodiscard]] void* allocate(
        std::size_t size, alloc_hint hint = {},
        std::source_location loc = std::source_location::current()) {
        if (size == 0) {
            size = 1;
        }

        if (hint.kind == pool_kind::fixed) {
            auto* pool = static_cast<memory_pool*>(hint.fixed_pool);
            if (!pool) {
                throw std::invalid_argument(
                    "memory_allocator: fixed hint requires fixed_pool");
            }
            if (size > pool->block_size()) {
                throw std::invalid_argument(
                    "memory_allocator: size exceeds fixed pool block_size");
            }
            void* p = pool->allocate(loc);
            track(p, live_entry{pool_kind::fixed, size, pool
#if UTILS_POOL_LEAK_CHECK >= 2
                                ,
                                loc
#endif
            });
            return p;
        }

        const bool force_raw = hint.kind == pool_kind::raw_new ||
                               hint.kind == pool_kind::large;
        const bool too_large =
            size > large_threshold_ || size > class_sizes_.back();
        if (force_raw ||
            (hint.kind == pool_kind::auto_select && too_large)) {
            void* p =
                ::operator new(size, std::align_val_t(alignof(std::max_align_t)));
            track(p, live_entry{pool_kind::raw_new, size, nullptr
#if UTILS_POOL_LEAK_CHECK >= 2
                                ,
                                loc
#endif
            });
            return p;
        }

        if (hint.kind == pool_kind::size_class && too_large) {
            throw std::invalid_argument(
                "memory_allocator: size exceeds size-class range");
        }

        const std::size_t index = class_index_for(size);
        memory_pool* pool = classes_[index].get();
        void* p = pool->allocate(loc);
        track(p, live_entry{pool_kind::size_class, size, pool
#if UTILS_POOL_LEAK_CHECK >= 2
                            ,
                            loc
#endif
        });
        return p;
    }

    void deallocate(void* p) noexcept {
        if (!p) {
            return;
        }
        live_entry entry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = live_.find(p);
            if (it == live_.end()) {
                return;
            }
            entry = it->second;
            live_.erase(it);
        }
        if (entry.kind == pool_kind::raw_new) {
            ::operator delete(p, std::align_val_t(alignof(std::max_align_t)));
        } else if (entry.pool) {
            entry.pool->deallocate(p);
        }
    }

    [[nodiscard]] stats snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        stats s;
        s.alloc_count = live_.size();
        for (const auto& [ptr, e] : live_) {
            (void)ptr;
            s.in_use_bytes += e.bytes;
            switch (e.kind) {
                case pool_kind::size_class:
                    s.size_class_bytes += e.bytes;
                    break;
                case pool_kind::raw_new:
                case pool_kind::large:
                    s.raw_new_bytes += e.bytes;
                    break;
                case pool_kind::fixed:
                    s.fixed_hint_bytes += e.bytes;
                    break;
                default:
                    break;
            }
        }
        return s;
    }

    [[nodiscard]] std::size_t large_threshold() const noexcept {
        return large_threshold_;
    }

    [[nodiscard]] const size_class_config& config() const noexcept {
        return config_;
    }

    [[nodiscard]] const std::vector<std::size_t>& class_sizes()
        const noexcept {
        return class_sizes_;
    }

    [[nodiscard]] std::size_t size_class_count() const noexcept {
        return class_sizes_.size();
    }

    /** 请求 size 会上取整到的档大小；超出最大档返回 0 */
    [[nodiscard]] std::size_t round_up_size(std::size_t size) const noexcept {
        if (size == 0) {
            size = 1;
        }
        for (std::size_t sz : class_sizes_) {
            if (size <= sz) {
                return sz;
            }
        }
        return 0;
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    void dump_leaks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        dump_leaks_to_unlocked(stderr);
    }
#endif

private:
    struct live_entry {
        pool_kind kind = pool_kind::auto_select;
        std::size_t bytes = 0;
        memory_pool* pool = nullptr;
#if UTILS_POOL_LEAK_CHECK >= 2
        std::source_location loc{};
#endif
    };

    std::size_t class_index_for(std::size_t size) const {
        const auto it =
            std::lower_bound(class_sizes_.begin(), class_sizes_.end(), size);
        if (it == class_sizes_.end()) {
            return class_sizes_.size() - 1;
        }
        return static_cast<std::size_t>(it - class_sizes_.begin());
    }

    void track(void* p, live_entry entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        live_.emplace(p, entry);
    }

    std::size_t in_use_bytes_unlocked() const {
        std::size_t n = 0;
        for (const auto& [ptr, e] : live_) {
            (void)ptr;
            n += e.bytes;
        }
        return n;
    }

#if UTILS_POOL_LEAK_CHECK >= 2
    void dump_leaks_to_unlocked(FILE* out) const {
        std::fprintf(out, "memory_allocator outstanding=%zu\n", live_.size());
        for (const auto& [ptr, e] : live_) {
            std::fprintf(out,
                         "  ptr=%p  bytes=%zu  kind=%d  %s:%u  %s\n", ptr,
                         e.bytes, static_cast<int>(e.kind), e.loc.file_name(),
                         e.loc.line(), e.loc.function_name());
        }
    }
#endif

    size_class_config config_;
    std::vector<std::size_t> class_sizes_;
    std::size_t large_threshold_;
    std::size_t blocks_per_chunk_;
    std::vector<std::unique_ptr<memory_pool>> classes_;
    mutable std::mutex mutex_;
    std::unordered_map<void*, live_entry> live_;
};

}  // namespace utils

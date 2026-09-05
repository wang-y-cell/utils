#pragma once

/**
 * memory_allocator — 无锁二级 size-class freelist（SGI 风格热路径）。
 *
 * API：allocate(n) / deallocate(p, n) —— 归还必须带分配时的字节数（或同档上取整）。
 * 线程安全由调用方保证。大于 mid_max 走 ::operator new。
 *
 * 泄漏档 UTILS_POOL_LEAK_CHECK：
 *   0 — 热路径无记账（目标：接近 SGI）
 *   1 — outstanding 计数；析构告警
 *   2 — ptr→(bytes, source_location)；dump_leaks()
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <source_location>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef UTILS_POOL_LEAK_CHECK
#define UTILS_POOL_LEAK_CHECK 0
#endif

namespace utils {

enum class size_class_preset {
    balanced,
    dense_small,
    compact,
};

struct size_class_config {
    std::size_t small_step = 8;
    std::size_t small_max = 128;
    std::size_t mid_step = 128;
    std::size_t mid_max = 4096;
    /** 0 → 使用 mid_max（最大池化档） */
    std::size_t large_threshold = 0;
    /** refill 时一次希望切出的块数（类似 SGI nobjs） */
    std::size_t refill_objects = 20;
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
                cfg.mid_max = 4224;
                break;
        }
        return cfg;
    }
};

namespace detail {

inline void validate_size_class_config(const size_class_config& cfg) {
    if (cfg.small_step == 0 || cfg.mid_step == 0) {
        throw std::invalid_argument("size_class_config: step must be > 0");
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
    if (cfg.refill_objects == 0) {
        throw std::invalid_argument(
            "size_class_config: refill_objects must be > 0");
    }
    if (cfg.max_classes == 0) {
        throw std::invalid_argument(
            "size_class_config: max_classes must be > 0");
    }
    if ((cfg.small_step & (cfg.small_step - 1)) != 0 ||
        (cfg.mid_step & (cfg.mid_step - 1)) != 0) {
        throw std::invalid_argument(
            "size_class_config: steps must be powers of two");
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
    if (sizes.empty() || sizes.size() > cfg.max_classes) {
        throw std::invalid_argument(
            "size_class_config: invalid class table size");
    }
    return sizes;
}

}  // namespace detail

class memory_allocator {
public:
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
          small_max_(cfg.small_max),
          small_step_(cfg.small_step),
          mid_step_(cfg.mid_step),
          small_count_(cfg.small_max / cfg.small_step),
          nclasses_(class_sizes_.size()) {
        if (large_threshold_ < class_sizes_.front()) {
            throw std::invalid_argument(
                "memory_allocator: large_threshold too small");
        }
        if (nclasses_ > kMaxClasses) {
            throw std::invalid_argument(
                "memory_allocator: too many size classes");
        }
        for (std::size_t i = 0; i < kMaxClasses; ++i) {
            free_lists_[i] = nullptr;
        }
    }

    ~memory_allocator() {
#if UTILS_POOL_LEAK_CHECK >= 1
        if (outstanding_ != 0) {
            std::fprintf(stderr,
                         "memory_allocator: destroy with %zu allocation(s) "
                         "still live\n",
                         outstanding_);
#if UTILS_POOL_LEAK_CHECK >= 2
            dump_leaks_to(stderr);
            report_leaks_to_file();
#endif
        }
#endif
        for (void* chunk : chunks_) {
            std::free(chunk);
        }
    }

    memory_allocator(const memory_allocator&) = delete;
    memory_allocator& operator=(const memory_allocator&) = delete;

    [[nodiscard]] void* allocate(std::size_t n
#if UTILS_POOL_LEAK_CHECK >= 2
                                 ,
                                 std::source_location loc =
                                     std::source_location::current()
#endif
    ) {
        if (n == 0) {
            n = 1;
        }

        if (n > large_threshold_) {
            void* p = ::operator new(
                n, std::align_val_t(alignof(std::max_align_t)));
#if UTILS_POOL_LEAK_CHECK >= 1
            track_alloc(p, n
#if UTILS_POOL_LEAK_CHECK >= 2
                        ,
                        loc
#endif
            );
#endif
            return p;
        }

        std::size_t bytes;
        std::size_t index;
        if (n <= small_max_) {
            bytes = (n + small_step_ - 1) & ~(small_step_ - 1);
            index = (bytes / small_step_) - 1;
        } else {
            const std::size_t over = n - small_max_;
            const std::size_t steps = (over + mid_step_ - 1) / mid_step_;
            bytes = small_max_ + steps * mid_step_;
            index = small_count_ + steps - 1;
        }

        free_node* result = free_lists_[index];
        if (result) {
            free_lists_[index] = result->next;
#if UTILS_POOL_LEAK_CHECK >= 1
            track_alloc(result, bytes
#if UTILS_POOL_LEAK_CHECK >= 2
                        ,
                        loc
#endif
            );
#endif
            return result;
        }
        void* p = refill(bytes, index);
#if UTILS_POOL_LEAK_CHECK >= 1
        track_alloc(p, bytes
#if UTILS_POOL_LEAK_CHECK >= 2
                    ,
                    loc
#endif
        );
#endif
        return p;
    }

    void deallocate(void* p, std::size_t n) noexcept {
        if (!p) {
            return;
        }
        if (n == 0) {
            n = 1;
        }

#if UTILS_POOL_LEAK_CHECK >= 1
        track_dealloc(p);
#endif

        if (n > large_threshold_) {
            ::operator delete(p, std::align_val_t(alignof(std::max_align_t)));
            return;
        }

        std::size_t bytes;
        std::size_t index;
        if (n <= small_max_) {
            bytes = (n + small_step_ - 1) & ~(small_step_ - 1);
            index = (bytes / small_step_) - 1;
        } else {
            const std::size_t over = n - small_max_;
            const std::size_t steps = (over + mid_step_ - 1) / mid_step_;
            bytes = small_max_ + steps * mid_step_;
            index = small_count_ + steps - 1;
        }

        auto* q = static_cast<free_node*>(p);
        q->next = free_lists_[index];
        free_lists_[index] = q;
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
        return nclasses_;
    }

    /** 上取整到档大小；超出最大池化档返回 0 */
    [[nodiscard]] std::size_t round_up_size(std::size_t n) const noexcept {
        if (n == 0) {
            n = 1;
        }
        if (n > large_threshold_) {
            return 0;
        }
        if (n <= small_max_) {
            return (n + small_step_ - 1) & ~(small_step_ - 1);
        }
        const std::size_t over = n - small_max_;
        const std::size_t steps = (over + mid_step_ - 1) / mid_step_;
        return small_max_ + steps * mid_step_;
    }

#if UTILS_POOL_LEAK_CHECK >= 1
    [[nodiscard]] std::size_t outstanding() const noexcept {
        return outstanding_;
    }
#endif

#if UTILS_POOL_LEAK_CHECK >= 2
    void dump_leaks() const { dump_leaks_to(stderr); }
#endif

private:
    union free_node {
        free_node* next;
        char data[1];
    };

    std::size_t index_for_rounded(std::size_t bytes) const noexcept {
        if (bytes <= small_max_) {
            return bytes / small_step_ - 1;
        }
        return small_count_ + (bytes - small_max_) / mid_step_ - 1;
    }

    void* refill(std::size_t bytes, std::size_t index) {
        int nobjs = static_cast<int>(config_.refill_objects);
        char* chunk = chunk_alloc(bytes, nobjs);
        if (nobjs == 1) {
            return chunk;
        }
        char* cur = chunk + bytes;
        free_lists_[index] = reinterpret_cast<free_node*>(cur);
        free_node* current = reinterpret_cast<free_node*>(cur);
        for (int i = 1; i < nobjs - 1; ++i) {
            char* next = cur + bytes;
            current->next = reinterpret_cast<free_node*>(next);
            current = reinterpret_cast<free_node*>(next);
            cur = next;
        }
        current->next = nullptr;
        return chunk;
    }

    std::size_t round_down_to_class(std::size_t n) const noexcept {
        if (n < small_step_) {
            return 0;
        }
        if (n <= small_max_) {
            return n & ~(small_step_ - 1);
        }
        const std::size_t over = n - small_max_;
        const std::size_t steps = over / mid_step_;
        if (steps == 0) {
            return small_max_;
        }
        return small_max_ + steps * mid_step_;
    }

    char* chunk_alloc(std::size_t size, int& nobjs) {
        const std::size_t total_bytes = size * static_cast<std::size_t>(nobjs);
        const std::size_t bytes_left =
            static_cast<std::size_t>(end_free_ - start_free_);

        if (bytes_left >= total_bytes) {
            char* result = start_free_;
            start_free_ += total_bytes;
            return result;
        }
        if (bytes_left >= size) {
            nobjs = static_cast<int>(bytes_left / size);
            const std::size_t got = size * static_cast<std::size_t>(nobjs);
            char* result = start_free_;
            start_free_ += got;
            return result;
        }

        if (bytes_left > 0) {
            const std::size_t leftover = round_down_to_class(bytes_left);
            if (leftover >= small_step_) {
                const std::size_t idx = index_for_rounded(leftover);
                auto* node = reinterpret_cast<free_node*>(start_free_);
                node->next = free_lists_[idx];
                free_lists_[idx] = node;
            }
        }

        std::size_t bytes_to_get =
            2 * total_bytes +
            ((heap_size_ >> 4) + small_step_ - 1) / small_step_ * small_step_;
        if (bytes_to_get < total_bytes) {
            bytes_to_get = total_bytes;
        }

        start_free_ = static_cast<char*>(std::malloc(bytes_to_get));
        if (!start_free_) {
            for (std::size_t i = index_for_rounded(size) + 1; i < nclasses_;
                 ++i) {
                free_node* p = free_lists_[i];
                if (p) {
                    free_lists_[i] = p->next;
                    start_free_ = reinterpret_cast<char*>(p);
                    end_free_ = start_free_ + class_sizes_[i];
                    return chunk_alloc(size, nobjs);
                }
            }
            throw std::bad_alloc();
        }

        chunks_.push_back(start_free_);
        heap_size_ += bytes_to_get;
        end_free_ = start_free_ + bytes_to_get;
        return chunk_alloc(size, nobjs);
    }

#if UTILS_POOL_LEAK_CHECK >= 1
    void track_alloc(void* p, std::size_t bytes
#if UTILS_POOL_LEAK_CHECK >= 2
                     ,
                     std::source_location loc
#endif
    ) {
        ++outstanding_;
#if UTILS_POOL_LEAK_CHECK >= 2
        sites_.emplace(p, site_entry{bytes, loc});
#else
        (void)p;
        (void)bytes;
#endif
    }

    void track_dealloc(void* p) noexcept {
        if (outstanding_ > 0) {
            --outstanding_;
        }
#if UTILS_POOL_LEAK_CHECK >= 2
        sites_.erase(p);
#else
        (void)p;
#endif
    }
#endif

#if UTILS_POOL_LEAK_CHECK >= 2
    struct site_entry {
        std::size_t bytes = 0;
        std::source_location loc{};
    };

    void dump_leaks_to(FILE* out) const {
        std::fprintf(out, "memory_allocator outstanding=%zu\n",
                     sites_.size());
        for (const auto& [ptr, e] : sites_) {
            std::fprintf(out, "  ptr=%p  bytes=%zu  %s:%u  %s\n", ptr,
                         e.bytes, e.loc.file_name(), e.loc.line(),
                         e.loc.function_name());
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

    std::unordered_map<void*, site_entry> sites_;
#endif

    size_class_config config_;
    std::vector<std::size_t> class_sizes_;
    static constexpr std::size_t kMaxClasses = 256;
    free_node* free_lists_[kMaxClasses]{};
    std::size_t large_threshold_ = 0;
    std::size_t small_max_ = 0;
    std::size_t small_step_ = 0;
    std::size_t mid_step_ = 0;
    std::size_t small_count_ = 0;
    std::size_t nclasses_ = 0;

    char* start_free_ = nullptr;
    char* end_free_ = nullptr;
    std::size_t heap_size_ = 0;
    std::vector<void*> chunks_;

#if UTILS_POOL_LEAK_CHECK >= 1
    std::size_t outstanding_ = 0;
#endif
};

}  // namespace utils

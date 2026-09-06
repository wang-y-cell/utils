#pragma once

/**
 * memory_allocator — 无锁多段 size-class freelist（SGI 风格热路径）。
 *
 * API：allocate(n) / deallocate(p, n) —— 归还必须带分配时的字节数（或同档上取整）。
 * 线程安全由调用方保证。大于最大池化档走 ::operator new。
 *
 * size-class 由若干 band 描述：每段有独立 step / max，支持 1 段或 N 段。
 * max_classes 默认 256；设为 0 表示不限制档数（freelist 动态分配）。
 *
 * 泄漏档 UTILS_POOL_LEAK_CHECK：
 *   0 — 热路径无记账（目标：接近 SGI）
 *   1 — outstanding；分开统计 rounding / class_table 浪费（dump_waste / 访问器）
 *   2 — ptr→(bytes, source_location)；dump_leaks()
 *
 * 浪费两项独立（勿混加）：
 *   rounding     — 历次 allocate 的 (档大小 - 请求) 累加，衡量步长是否过粗
 *   class_table  — freelist 头表 + class_sizes + bands 元数据，衡量档数是否过多
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

#if defined(__GNUC__) || defined(__clang__)
#define UTILS_ALLOC_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define UTILS_ALLOC_ALWAYS_INLINE __forceinline
#else
#define UTILS_ALLOC_ALWAYS_INLINE inline
#endif

namespace utils {

enum class size_class_preset {
    balanced,
    dense_small,
    compact,
};

/** 一段连续档：从上一上界之后按 step 递增到 max（含）。 */
struct size_class_band {
    std::size_t step = 8;
    std::size_t max = 128;
};

struct size_class_config {
    /** 至少 1 段；max 严格递增。默认等价原 balanced 两级。 */
    std::vector<size_class_band> bands = {{8, 128}, {128, 4096}};
    /** 0 → 使用最后一档大小（最大池化档） */
    std::size_t large_threshold = 0;
    /** refill 时一次希望切出的块数（类似 SGI nobjs） */
    std::size_t refill_objects = 20;
    /** 档数软上限；0 = 不限制（freelist 按实际档数动态分配，消耗由调用方承担） */
    std::size_t max_classes = 256;

    [[nodiscard]] static size_class_config from_bands(
        std::vector<size_class_band> bands) {
        size_class_config cfg;
        cfg.bands = std::move(bands);
        return cfg;
    }

    /** 便捷：经典两级（small + mid） */
    [[nodiscard]] static size_class_config from_two_level(
        std::size_t small_step, std::size_t small_max, std::size_t mid_step,
        std::size_t mid_max) {
        return from_bands({{small_step, small_max}, {mid_step, mid_max}});
    }

    [[nodiscard]] static size_class_config from_preset(
        size_class_preset preset) {
        switch (preset) {
            case size_class_preset::balanced:
                return from_two_level(8, 128, 128, 4096);
            case size_class_preset::dense_small:
                return from_two_level(8, 256, 128, 4096);
            case size_class_preset::compact:
                return from_two_level(16, 128, 256, 4224);
        }
        return from_two_level(8, 128, 128, 4096);
    }
};

namespace detail {

inline void validate_size_class_config(const size_class_config& cfg) {
    if (cfg.bands.empty()) {
        throw std::invalid_argument("size_class_config: bands must be non-empty");
    }
    if (cfg.refill_objects == 0) {
        throw std::invalid_argument(
            "size_class_config: refill_objects must be > 0");
    }

    std::size_t prev_max = 0;
    for (std::size_t i = 0; i < cfg.bands.size(); ++i) {
        const auto& b = cfg.bands[i];
        if (b.step == 0) {
            throw std::invalid_argument("size_class_config: step must be > 0");
        }
        if (b.max == 0) {
            throw std::invalid_argument("size_class_config: max must be > 0");
        }
        if ((b.step & (b.step - 1)) != 0) {
            throw std::invalid_argument(
                "size_class_config: steps must be powers of two");
        }
        if (b.max <= prev_max) {
            throw std::invalid_argument(
                "size_class_config: band max must be strictly increasing");
        }
        if ((b.max - prev_max) % b.step != 0) {
            throw std::invalid_argument(
                "size_class_config: (max - prev_max) must be multiple of step");
        }
        prev_max = b.max;
    }
}

inline std::vector<std::size_t> build_class_sizes(
    const size_class_config& cfg) {
    validate_size_class_config(cfg);
    std::vector<std::size_t> sizes;
    std::size_t prev_max = 0;
    for (const auto& b : cfg.bands) {
        for (std::size_t s = prev_max + b.step; s <= b.max; s += b.step) {
            sizes.push_back(s);
        }
        prev_max = b.max;
    }
    if (sizes.empty()) {
        throw std::invalid_argument(
            "size_class_config: class table is empty");
    }
    if (cfg.max_classes != 0 && sizes.size() > cfg.max_classes) {
        throw std::invalid_argument(
            "size_class_config: too many size classes (check step/max vs "
            "max_classes, or set max_classes=0)");
    }
    return sizes;
}

struct band_runtime {
    std::size_t step = 0;
    std::size_t max = 0;
    std::size_t prev_max = 0;
    std::size_t first_index = 0;
};

inline std::vector<band_runtime> build_band_runtime(
    const size_class_config& cfg) {
    std::vector<band_runtime> out;
    out.reserve(cfg.bands.size());
    std::size_t prev_max = 0;
    std::size_t index_base = 0;
    for (const auto& b : cfg.bands) {
        band_runtime r;
        r.step = b.step;
        r.max = b.max;
        r.prev_max = prev_max;
        r.first_index = index_base;
        const std::size_t count = (b.max - prev_max) / b.step;
        index_base += count;
        prev_max = b.max;
        out.push_back(r);
    }
    return out;
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
        : config_(std::move(cfg)),
          class_sizes_(detail::build_class_sizes(config_)),
          bands_(detail::build_band_runtime(config_)),
          large_threshold_(config_.large_threshold == 0 ? class_sizes_.back()
                                                       : config_.large_threshold),
          min_step_(bands_.front().step),
          nclasses_(class_sizes_.size()),
          free_lists_(nclasses_, nullptr) {
        if (large_threshold_ < class_sizes_.front()) {
            throw std::invalid_argument(
                "memory_allocator: large_threshold too small");
        }
#if UTILS_POOL_LEAK_CHECK >= 1
        class_table_bytes_ = compute_class_table_bytes();
#endif
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

    [[nodiscard]] UTILS_ALLOC_ALWAYS_INLINE void* allocate(std::size_t n
#if UTILS_POOL_LEAK_CHECK >= 2
                                                           ,
                                                           std::source_location
                                                               loc = std::
                                                                   source_location::
                                                                       current()
#endif
    ) {
        if (n == 0) [[unlikely]] {
            n = 1;
        }

        if (n > large_threshold_) [[unlikely]] {
            void* p = ::operator new(
                n, std::align_val_t(alignof(std::max_align_t)));
#if UTILS_POOL_LEAK_CHECK >= 1
            track_alloc(p, n, n
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
        map_size(n, bytes, index);

        free_node* result = free_lists_[index];
        if (result) [[likely]] {
            free_lists_[index] = result->next;
#if UTILS_POOL_LEAK_CHECK >= 1
            track_alloc(result, bytes, n
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
        track_alloc(p, bytes, n
#if UTILS_POOL_LEAK_CHECK >= 2
                    ,
                    loc
#endif
        );
#endif
        return p;
    }

    UTILS_ALLOC_ALWAYS_INLINE void deallocate(void* p,
                                              std::size_t n) noexcept {
        if (!p) [[unlikely]] {
            return;
        }
        deallocate_unchecked(p, n);
    }

    /** 调用方保证 p 非空 */
    UTILS_ALLOC_ALWAYS_INLINE void deallocate_unchecked(
        void* p, std::size_t n) noexcept {
        if (n == 0) [[unlikely]] {
            n = 1;
        }

#if UTILS_POOL_LEAK_CHECK >= 1
        track_dealloc(p);
#endif

        if (n > large_threshold_) [[unlikely]] {
            ::operator delete(p, std::align_val_t(alignof(std::max_align_t)));
            return;
        }

        std::size_t bytes;
        std::size_t index;
        map_size(n, bytes, index);

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
    [[nodiscard]] std::size_t band_count() const noexcept {
        return bands_.size();
    }

    /** 上取整到档大小；超出最大池化档返回 0 */
    [[nodiscard]] std::size_t round_up_size(std::size_t n) const noexcept {
        if (n == 0) {
            n = 1;
        }
        if (n > large_threshold_) {
            return 0;
        }
        std::size_t bytes;
        std::size_t index;
        map_size(n, bytes, index);
        (void)index;
        return bytes;
    }

#if UTILS_POOL_LEAK_CHECK >= 1
    [[nodiscard]] std::size_t outstanding() const noexcept {
        return outstanding_;
    }
    /** 历次池化分配的 (档大小 - 请求) 累加；与 class_table 分开 */
    [[nodiscard]] std::size_t rounding_waste() const noexcept {
        return rounding_waste_;
    }
    /** freelist 头表 + class_sizes + bands 元数据字节；与 rounding 分开 */
    [[nodiscard]] std::size_t class_table_bytes() const noexcept {
        return class_table_bytes_;
    }
    void dump_waste() const { report_waste_to(stderr); }
#endif

#if UTILS_POOL_LEAK_CHECK >= 2
    void dump_leaks() const { dump_leaks_to(stderr); }
#endif

private:
    union free_node {
        free_node* next;
        char data[1];
    };

    UTILS_ALLOC_ALWAYS_INLINE void map_size(std::size_t n, std::size_t& bytes,
                                            std::size_t& index) const noexcept {
        for (const auto& b : bands_) {
            if (n <= b.max) {
                const std::size_t over = n - b.prev_max;
                const std::size_t steps = (over + b.step - 1) / b.step;
                bytes = b.prev_max + steps * b.step;
                index = b.first_index + steps - 1;
                return;
            }
        }
        // n <= large_threshold_ 时不应到达
        bytes = class_sizes_.back();
        index = nclasses_ - 1;
    }

    std::size_t index_for_rounded(std::size_t bytes) const noexcept {
        for (const auto& b : bands_) {
            if (bytes <= b.max) {
                return b.first_index + (bytes - b.prev_max) / b.step - 1;
            }
        }
        return nclasses_ - 1;
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
        if (n < min_step_) {
            return 0;
        }
        for (const auto& b : bands_) {
            if (n <= b.max) {
                const std::size_t over = n - b.prev_max;
                const std::size_t steps = over / b.step;
                if (steps == 0) {
                    return b.prev_max;
                }
                return b.prev_max + steps * b.step;
            }
        }
        return class_sizes_.back();
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
            if (leftover >= min_step_) {
                const std::size_t idx = index_for_rounded(leftover);
                auto* node = reinterpret_cast<free_node*>(start_free_);
                node->next = free_lists_[idx];
                free_lists_[idx] = node;
            }
        }

        std::size_t bytes_to_get =
            2 * total_bytes +
            ((heap_size_ >> 4) + min_step_ - 1) / min_step_ * min_step_;
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
    [[nodiscard]] std::size_t compute_class_table_bytes() const noexcept {
        return free_lists_.size() * sizeof(free_node*) +
               class_sizes_.size() * sizeof(std::size_t) +
               bands_.size() * sizeof(detail::band_runtime);
    }

    void report_waste_to(FILE* out) const {
        char round_h[64];
        char table_h[64];
        const auto fmt = [](std::size_t n, char* buf, std::size_t nbuf) {
            if (n >= (std::size_t{1} << 30)) {
                std::snprintf(buf, nbuf, "%.3f GB",
                              static_cast<double>(n) / 1073741824.0);
            } else if (n >= (std::size_t{1} << 20)) {
                std::snprintf(buf, nbuf, "%.3f MB",
                              static_cast<double>(n) / 1048576.0);
            } else if (n >= (std::size_t{1} << 10)) {
                std::snprintf(buf, nbuf, "%.3f KB",
                              static_cast<double>(n) / 1024.0);
            } else {
                std::snprintf(buf, nbuf, "%zu B", n);
            }
        };
        fmt(rounding_waste_, round_h, sizeof(round_h));
        fmt(class_table_bytes_, table_h, sizeof(table_h));
        std::fprintf(out,
                     "memory_allocator waste: rounding=%zu (%s)  "
                     "class_table=%zu (%s)  (classes=%zu)\n",
                     rounding_waste_, round_h, class_table_bytes_, table_h,
                     nclasses_);
    }

    void track_alloc(void* p, std::size_t bytes, std::size_t requested
#if UTILS_POOL_LEAK_CHECK >= 2
                     ,
                     std::source_location loc
#endif
    ) {
        ++outstanding_;
        if (bytes > requested) {
            rounding_waste_ += bytes - requested;
        }
#if UTILS_POOL_LEAK_CHECK >= 2
        sites_.emplace(p, site_entry{bytes, loc});
#else
        (void)p;
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
    std::vector<detail::band_runtime> bands_;
    std::size_t large_threshold_ = 0;
    std::size_t min_step_ = 0;
    std::size_t nclasses_ = 0;
    std::vector<free_node*> free_lists_;

    char* start_free_ = nullptr;
    char* end_free_ = nullptr;
    std::size_t heap_size_ = 0;
    std::vector<void*> chunks_;

#if UTILS_POOL_LEAK_CHECK >= 1
    std::size_t outstanding_ = 0;
    std::size_t rounding_waste_ = 0;
    std::size_t class_table_bytes_ = 0;
#endif
};

}  // namespace utils

#undef UTILS_ALLOC_ALWAYS_INLINE

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace utils {

/**
 * 通用线程池（header-only）。
 *
 * 取舍说明：
 * - 单队列 + mutex：实现清晰，覆盖绝大多数业务负载。
 * - 有界队列提供背压（max_queue_size == 0 表示无界）。
 * - submit 异常进入 future；add_task 为 fire-and-forget。
 * - worker 使用 detach + 存活计数，缩容在锁内“占位退出”，避免超退。
 */
class thread_pool {
public:
    /// @param thread_count 工作线程数（至少为 1）
    /// @param max_queue_size 队列上限；0 表示不限制
    explicit thread_pool(std::size_t thread_count,
                         std::size_t max_queue_size = 0)
        : max_queue_size_(max_queue_size) {
        if (thread_count == 0) {
            thread_count = 1;
        }
        target_workers_ = thread_count;
        for (std::size_t i = 0; i < thread_count; ++i) {
            launch_worker();
        }
    }

    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    thread_pool& operator=(thread_pool&&) = delete;

    ~thread_pool() {
        shutdown(/*wait_for_tasks=*/true);
    }

    /// 提交任务并返回 future；异常在 future.get() 时抛出
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using result_t = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        auto packed = std::make_shared<std::packaged_task<result_t()>>(
            [func = std::forward<F>(f),
             tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(tup));
            });

        auto fut = packed->get_future();
        enqueue([packed] { (*packed)(); });
        return fut;
    }

    /// 无返回值提交（兼容旧接口）
    template <class F, class... Args>
    void add_task(F&& f, Args&&... args) {
        enqueue(
            [func = std::forward<F>(f),
             tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                std::apply(std::move(func), std::move(tup));
            });
    }

    /// 非阻塞提交：已停止或队列满时返回 false
    template <class F, class... Args>
    bool try_add_task(F&& f, Args&&... args) {
        return try_enqueue(
            [func = std::forward<F>(f),
             tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                std::apply(std::move(func), std::move(tup));
            });
    }

    /// 调整目标线程数；缩容在 worker 空闲时生效
    void resize(std::size_t thread_count) {
        if (thread_count == 0) {
            thread_count = 1;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }

        if (thread_count == target_workers_) {
            return;
        }

        if (thread_count > target_workers_) {
            const std::size_t add = thread_count - target_workers_;
            target_workers_ = thread_count;
            lock.unlock();
            for (std::size_t i = 0; i < add; ++i) {
                launch_worker();
            }
            return;
        }

        target_workers_ = thread_count;
        lock.unlock();
        task_cv_.notify_all();
    }

    /// 等到队列为空且没有正在执行的任务
    void wait() const {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_cv_.wait(lock, [this] {
            return tasks_.empty() && active_tasks_ == 0;
        });
    }

    /**
     * 关闭线程池。可重复调用。
     * @param wait_for_tasks true 处理完已入队任务；false 丢弃未执行任务
     */
    void shutdown(bool wait_for_tasks = true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_) {
                done_cv_.wait(lock, [this] { return outstanding_threads_ == 0; });
                return;
            }
            stopping_ = true;
            target_workers_ = 0;
            if (!wait_for_tasks) {
                std::queue<task_t> empty;
                tasks_.swap(empty);
            }
        }
        task_cv_.notify_all();
        space_cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this] { return outstanding_threads_ == 0; });
    }

    std::size_t thread_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return alive_workers_;
    }

    std::size_t target_thread_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_workers_;
    }

    std::size_t pending_tasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    bool stopped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

private:
    using task_t = std::function<void()>;

    void launch_worker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++outstanding_threads_;
        }
        try {
            std::thread([this] {
                try {
                    worker_loop();
                } catch (...) {
                }
                std::lock_guard<std::mutex> lock(mutex_);
                --outstanding_threads_;
                if (outstanding_threads_ == 0) {
                    done_cv_.notify_all();
                }
            }).detach();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            --outstanding_threads_;
            if (outstanding_threads_ == 0) {
                done_cv_.notify_all();
            }
            throw;
        }
    }

    bool queue_full_unlocked() const {
        return max_queue_size_ != 0 && tasks_.size() >= max_queue_size_;
    }

    bool need_shrink_unlocked() const {
        return alive_workers_ > target_workers_;
    }

    void notify_idle_unlocked() const {
        if (tasks_.empty() && active_tasks_ == 0) {
            idle_cv_.notify_all();
        }
    }

    /// 在已持有 mutex_ 时调用：占位退出，避免多个 worker 同时超退
    void leave_unlocked() {
        --alive_workers_;
        if (alive_workers_ == 0) {
            done_cv_.notify_all();
        }
        notify_idle_unlocked();
    }

    void enqueue(task_t task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            space_cv_.wait(lock, [this] {
                return stopping_ || !queue_full_unlocked();
            });
            if (stopping_) {
                throw std::runtime_error("submit on stopped thread_pool");
            }
            tasks_.push(std::move(task));
        }
        task_cv_.notify_one();
    }

    bool try_enqueue(task_t task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || queue_full_unlocked()) {
                return false;
            }
            tasks_.push(std::move(task));
        }
        task_cv_.notify_one();
        return true;
    }

    void worker_loop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++alive_workers_;
        }

        for (;;) {
            task_t task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                task_cv_.wait(lock, [this] {
                    return stopping_ || !tasks_.empty() || need_shrink_unlocked();
                });

                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop();
                    ++active_tasks_;
                    if (max_queue_size_ != 0) {
                        space_cv_.notify_one();
                    }
                } else {
                    // 无任务：停机或缩容时占位退出
                    leave_unlocked();
                    return;
                }
            }

            try {
                task();
            } catch (...) {
                // add_task 异常在此结束；submit 的异常已进入 future。
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_tasks_;
                notify_idle_unlocked();

                if (tasks_.empty() && (stopping_ || need_shrink_unlocked())) {
                    leave_unlocked();
                    return;
                }
            }
        }
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable task_cv_;
    mutable std::condition_variable space_cv_;
    mutable std::condition_variable idle_cv_;
    mutable std::condition_variable done_cv_;

    std::queue<task_t> tasks_;

    std::size_t max_queue_size_ = 0;
    std::size_t target_workers_ = 0;
    std::size_t alive_workers_ = 0;
    std::size_t outstanding_threads_ = 0;
    std::size_t active_tasks_ = 0;
    bool stopping_ = false;
};

}  // namespace utils

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
        //如果参数是thread_count设置为0，则将他设置为1，至少要有一个线程
        if (thread_count == 0) {
            thread_count = 1;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        //如果当前线程池设置为停止，则返回
        if (stopping_) {
            return;
        }

        //如果目标线程数和参数一样，则返回
        if (thread_count == target_workers_) {
            return;
        }

        //如果当前线程数小于参数，则添加线程
        if (thread_count > target_workers_) {
            //算出要添加的线程数
            const std::size_t add = thread_count - target_workers_;
            target_workers_ = thread_count;
            lock.unlock(); //在创建这个线程的时候需要释放锁，不然会死锁
            for (std::size_t i = 0; i < add; ++i) {
                launch_worker();
            }
            return;
        }

        target_workers_ = thread_count;
        lock.unlock();
        task_cv_.notify_all(); //通知所有等待任务执行的线程，可以执行任务了
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
            //如果stop标志位设置，等待所有线程结束，
            // 然后返回,一般这个被设置表示这个函数在其他地方被调用了
            if (stopping_) {
                done_cv_.wait(lock, [this] { return outstanding_threads_ == 0; });
                return;
            }
            stopping_ = true; //设置stop标志位
            target_workers_ = 0; //设置目标线程数为0
            if (!wait_for_tasks) { //如果不需要等待任务执行完成就直接清空队列
                std::queue<task_t> empty;
                tasks_.swap(empty);
            }
        }
        task_cv_.notify_all();
        space_cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this] { return outstanding_threads_ == 0; });
    }

    /// 获得正在loop的线程数
    std::size_t thread_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return alive_workers_;
    }

    /// 获得目标线程数
    std::size_t target_thread_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_workers_;
    }

    /// 获得等待执行的任务数
    std::size_t pending_tasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    /// 获得当前线程池是否停止
    bool stopped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

private:
    /// 任务类型,无返回值
    using task_t = std::function<void()>;

    ///创建一个线程，并让他运行loop
    void launch_worker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++outstanding_threads_;
        }
        try {
            std::thread([this] {
                try {
                    worker_loop();
                } catch (...) { //忽略任何错误
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

    /// 获得队列是否满
    bool queue_full_unlocked() const {
        ///如果队列不为无限大且等待执行的任务数量大小大于队列大小，则队列满
        return max_queue_size_ != 0 && tasks_.size() >= max_queue_size_;
    }

    /// 获得是否需要缩容
    bool need_shrink_unlocked() const {
        //如果正在loop的线程数大于目标线程数，则需要缩容
        return alive_workers_ > target_workers_;
    }

    void notify_idle_unlocked() const {
        //如果队列没有任务了而且没有正在执行的任务了,唤醒等待线程空闲的线程
        if (tasks_.empty() && active_tasks_ == 0) {
            idle_cv_.notify_all();
        }
    }

    /// 在已持有 mutex_ 时调用：占位退出，避免多个 worker 同时超退
    void leave_unlocked() {
        --alive_workers_; //正在loop的线程数减1
        if (alive_workers_ == 0) { //如果没有正在loop的线程
            done_cv_.notify_all(); //通知所有等待线程退出的线程，线程池已经退出
        }
        notify_idle_unlocked();
    }

    void enqueue(task_t task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            //如果当前队列满了，就等待
            space_cv_.wait(lock, [this] {
                //当当前线程池设置为停止，或者队列不满，就不再等待
                return stopping_ || !queue_full_unlocked();
            });
            //如果是停止了,则抛出异常
            if (stopping_) {
                throw std::runtime_error("submit on stopped thread_pool");
            }
            //添加任务到队列中
            tasks_.push(std::move(task));
        }
        task_cv_.notify_one();
    }

    //尝试添加任务到队列中,如果队列满了或者线程停止了就不添加，返回false
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

    //线程池中每个线程的loop
    void worker_loop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++alive_workers_; //添加正在loop的线程
        }

        for (;;) {
            task_t task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                task_cv_.wait(lock, [this] {
                    //当当前线程池设置为停止，或者任务队列不为空，或者需要缩容时,就不再等待
                    return stopping_ || !tasks_.empty() || need_shrink_unlocked();
                });

                //如果是队列非空，弹出这个任务，设置正在执行任务数加1
                //如果线程池设置队列大小不是无限的，则通知space_cv_，让其他线程可以继续添加任务
                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop();
                    ++active_tasks_;
                    if (max_queue_size_ != 0) {
                        //唤醒因为队列满了导致等待的线程，当前队列这个任务拿出
                        //所以当前队列有空位
                        space_cv_.notify_one();
                    }
                } else {
                    // 无任务：停机或缩容时占位退出
                    leave_unlocked();
                    return; //这个线程退出
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
    mutable std::condition_variable task_cv_; //等待队列是否有可执行任务的条件变量
    mutable std::condition_variable space_cv_; //队列是否有空间添加任务的条件变量
    mutable std::condition_variable idle_cv_; //等待线程空闲的条件变量
    mutable std::condition_variable done_cv_; //等待所有线程退出的条件变量

    std::queue<task_t> tasks_;

    std::size_t max_queue_size_ = 0;
    std::size_t target_workers_ = 0; //目标线程数,用来设置目前等待任务的线程数量
    std::size_t alive_workers_ = 0; //正在loop的线程
    std::size_t outstanding_threads_ = 0; //正在运行的线程数,一般和alive_workers_相等
    std::size_t active_tasks_ = 0; //正在执行的任务数
    bool stopping_ = false;
};

}  // namespace utils

#include "./../include/thread_pool.h"

thread_pool::thread_pool(int numThreads) : stop(false) {
    resize(numThreads);
}

thread_pool::~thread_pool() {
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop = true;
    }
    condition.notify_all();
    for (auto& x : threads) {
        if (x.joinable()) {
            x.join();
        }
    }
}


// 无返回值的任务提交（保持原有接口兼容）
template<typename F, typename... Args>
void thread_pool::add_task(F&& f, Args&&... args) {
    {
        std::unique_lock<std::mutex> lock(mtx);
        if (stop) throw std::runtime_error("add_task on stopped thread pool");
        tasks.emplace(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    }
    condition.notify_one();
}

// 支持返回值和异常捕获的任务提交
template<typename F, typename... Args>
auto thread_pool::submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
    using return_type = decltype(f(args...));
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(mtx);
        if (stop) throw std::runtime_error("submit on stopped thread pool");
        // 包装任务，确保异常安全
        tasks.emplace([task]() {
            try {
                (*task)();
            } catch (const std::exception& e) {
                std::cerr << "Task exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Task unknown exception" << std::endl;
            }
        });
    }
    condition.notify_one();
    return res;
}

// 动态调整线程池大小
void thread_pool::resize(int numThreads) {
    std::unique_lock<std::mutex> lock(mtx);
    int currentThreads = static_cast<int>(threads.size());
    if (numThreads == currentThreads) return;

    if (numThreads > currentThreads) {
        // 增加线程
        for (int i = currentThreads; i < numThreads; ++i) {
            threads.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        condition.wait(lock, [this] {
                            return !tasks.empty() || stop;
                        });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    // 锁外执行，包含异常安全包装
                    try {
                        task();
                    } catch (const std::exception& e) {
                        std::cerr << "Task exception: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "Task unknown exception" << std::endl;
                    }
                }
            });
        }
    } else {
        // 减少线程：标记停止并通知所有线程，由线程自身检测退出
        // 注意：实际减少线程数需要更复杂的机制，此处简化为仅支持增加
        // 如需完整支持减少，需引入线程状态管理和优雅退出协议
        std::cerr << "Warning: Thread pool shrink not fully supported in this implementation" << std::endl;
    }
}

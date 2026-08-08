#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <iostream>

class thread_pool {
public:
    thread_pool(int numThreads);

    ~thread_pool(); 

    // 无返回值的任务提交（保持原有接口兼容）
    template<typename F, typename... Args>
    void add_task(F&& f, Args&&... args);

    // 支持返回值和异常捕获的任务提交
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>;

    // 动态调整线程池大小
    void resize(int numThreads);

private:
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable condition;
    bool stop;
};
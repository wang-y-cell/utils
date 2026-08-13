#pragma once

/**
 * channel<T> — 有界/无界 MPSC 消息队列（C++20）
 * 队列可以无限增长,也可以有范围,范围就是capacity_
 *
 * capacity == 0 表示无界。close 后 send 失败；recv 在排空后返回 nullopt。
 *
 *   utils::channel<int> ch(64);
 *   ch.send(1);
 *   auto v = ch.recv();  // optional
 */

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace utils {

template <class T>
class channel {
public:
    /// @param capacity 0 = 无界
    explicit channel(std::size_t capacity = 0) : capacity_(capacity) {}

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;
    channel(channel&&) = delete;
    channel& operator=(channel&&) = delete;

    /** 阻塞直到入队成功或已关闭；关闭时返回 false */
    bool send(T value) {
        {
            //在发送一个数据的时候先阻塞住，直到队列有空闲位置
            std::unique_lock<std::mutex> lock(mutex_);
            not_full_.wait(lock, [this] {
                return closed_ || !full_unlocked(); //首先先运行这段谓词代码,谓词为真直接向下执行
            });
            if (closed_) {
                return false;
            }
            queue_.push(std::move(value));
        }
        not_empty_.notify_one(); // 通知消费者有数据了
        return true;
    }

    /** 非阻塞发送；满或已关闭返回 false */
    bool try_send(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || full_unlocked()) {
                return false;
            }
            queue_.push(std::move(value));
        }
        not_empty_.notify_one();
        return true;
    }

    /** 阻塞直到有数据或关闭且排空；关闭且空返回 nullopt */
    [[nodiscard]] std::optional<T> recv() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] {
            return closed_ || !queue_.empty();
        });
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    /** @brief 尝试获得一个数据, 如果队列为空就返回空,否则返回队列中的数据并唤醒一个非满条件变量 */
    [[nodiscard]] std::optional<T> try_recv() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return value;
    }

    /** @brief 关闭通道, 设置closed_为true, 并唤醒所有等待的消费者和生产者 */
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /** @brief 检查通道是否已关闭 */
    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    /** @brief 获取队列中的数据个数 */
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /** @brief 获取队列的容量 */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    // 无锁检查队列是否已满,如果当前队列是优先队列并且队列目前数据的大小大于等于容量,则认为队列已满
    bool full_unlocked() const {
        return capacity_ != 0 && queue_.size() >= capacity_;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_; //非空条件变量,当这个条件变量被唤醒表示队列中有数据了,通知消费者可以消费了
    std::condition_variable not_full_; //非满条件变量,当这个条件变量被唤醒表示队列有空闲位置了,通知生产者可以生产了
    std::queue<T> queue_;
    std::size_t capacity_ = 0;
    bool closed_ = false;
};

}  // namespace utils

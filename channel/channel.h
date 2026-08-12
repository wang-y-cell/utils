#pragma once

/**
 * Channel<T> — 有界/无界 MPSC 消息队列（C++20）
 *
 * capacity == 0 表示无界。close 后 send 失败；recv 在排空后返回 nullopt。
 *
 *   utils::Channel<int> ch(64);
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
class Channel {
public:
    /// @param capacity 0 = 无界
    explicit Channel(std::size_t capacity = 0) : capacity_(capacity) {}

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    /** 阻塞直到入队成功或已关闭；关闭时返回 false */
    bool send(T value) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_full_.wait(lock, [this] {
                return closed_ || !full_unlocked();
            });
            if (closed_) {
                return false;
            }
            queue_.push(std::move(value));
        }
        not_empty_.notify_one();
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

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    bool full_unlocked() const {
        return capacity_ != 0 && queue_.size() >= capacity_;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::queue<T> queue_;
    std::size_t capacity_ = 0;
    bool closed_ = false;
};

}  // namespace utils

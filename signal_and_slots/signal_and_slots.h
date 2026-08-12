#pragma once

/**
 * SignalAndSlots — 轻量 Qt 风格信号/槽 + 事件循环（C++17 header-only）
 *
 * 日常用法：
 *   #include "signal_and_slots.h"
 *   using namespace qto;
 *
 * 能力：
 *   - ConnectionType: Direct / Queued / Auto
 *   - Connection / ScopedConnection（可断开、RAII）
 *   - Object 析构自动断开入站连接，槽内校验存活，避免悬空
 *   - Object::deleteLater + object_uptr / object_sptr / object_wptr
 *   - connect / emit 线程安全；connect 支持裸指针与智能指针（非拥有观察）
 *   - EventLoop：post / 延迟定时器 / 周期定时器 / processEvents
 *   - WorkerThread：一线程一循环
 *   - CoreApplication / ensure_thread_loop：每线程默认 EventLoop（仿 QThread 亲和）
 *   - connect 语法糖
 *   - invoke：把可调用对象投递到目标 Object 所在线程
 *
 * 使用注意：
 *   1) 跨线程 Object 销毁前先 WorkerThread::stop() / 排空队列
 *   2) 派生类若可能被跨线程回调，析构函数第一行调用 invalidate()
 *       （堆对象优先 object_uptr / deleteLater，可减少手动 invalidate）
 *   3) 跨线程 Direct 会自动降级为 Queued；Queued 且无 loop 则丢弃
 *   4) 禁止在工作线程里调用 WorkerThread::stop() 期望 join 自己
 *   5) 主线程建议先构造 CoreApplication（或依赖 Object 内 ensure_thread_loop）
 *   6) 堆上 Object 建议只用 object_uptr/object_sptr 拥有；connect 仅观察不延长寿命
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qto {

enum class ConnectionType {
    Direct,  // 同步：在 emit 所在线程执行
    Queued,  // 异步：投递到接收者所属 EventLoop
    Auto     // 同线程 Direct，跨线程 Queued
};

class EventLoop;
class Object;

// 线程默认 loop（亲和身份，可不 run）
inline thread_local EventLoop* tls_default_loop = nullptr;
// 当前正在 run/processEvents 的 loop
inline thread_local EventLoop* tls_running_loop = nullptr;

/// 当前线程 loop：优先返回正在泵的，否则返回默认亲和 loop
inline EventLoop* current_thread_loop() noexcept {
    if (tls_running_loop) return tls_running_loop;
    return tls_default_loop;
}

// 前向声明；定义在 EventLoop 类之后
inline EventLoop* ensure_thread_loop();

// =============================================================================
// EventLoop
// =============================================================================
class EventLoop {
public:
    using Clock = std::chrono::steady_clock;
    using Task = std::function<void()>;
    using TimerId = std::uint64_t;

    EventLoop() = default;
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    ~EventLoop() { stop(); }

    /** @brief 将任务添加到任务队列中 */
    void post(Task task) {
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    /// 延迟执行。delay <= 0 等价于 post。返回可用 cancelTimer 取消的 id（post 路径为 0）。
    TimerId postDelayed(Clock::duration delay, Task task) {
        if (!task) return 0;
        if (delay <= Clock::duration::zero()) {
            post(std::move(task));
            return 0;
        }
        TimerId id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return 0;
            id = next_timer_id_++;
            timers_.push(TimerItem{Clock::now() + delay, id, std::move(task),
                                   Clock::duration::zero()});
        }
        cv_.notify_one();
        return id;
    }

    /// 周期执行；首次在 interval 之后触发。
    TimerId postPeriodic(Clock::duration interval, Task task) {
        if (!task || interval <= Clock::duration::zero()) return 0;
        TimerId id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return 0;
            id = next_timer_id_++;
            timers_.push(TimerItem{Clock::now() + interval, id, std::move(task),
                                   interval});
        }
        cv_.notify_one();
        return id;
    }

    void cancelTimer(TimerId id) {
        if (id == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.insert(id);
    }

    /** @brief 运行事件循环 */
    void run() {
        EventLoop* prev_running = tls_running_loop;
        tls_running_loop = this;
        if (!tls_default_loop) tls_default_loop = this;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = true;
        }

        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                for (;;) {
                    flush_due_timers_unlocked();
                    if (!tasks_.empty()) { //任务队列有任务
                        task = std::move(tasks_.front());
                        tasks_.pop();
                        break;
                    }
                    //任务队列没有任务了
                    if (!running_) {
                        tls_running_loop = prev_running;
                        return;
                    }
                    if (!timers_.empty()) { //没有任务，但是有定时任务，等待
                        cv_.wait_until(lock, timers_.top().when);
                    } else {
                        cv_.wait(lock); //任务队列没有任务，也没有定时任务
                    }
                }
            }
            if (task) task(); //执行任务
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
    }

    bool isRunning() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    /**  
     * @brief 处理已到期的定时器与已排队任务；可选最长等待。会临时设置 tls。
     * 
     */
    void processEvents(
        std::optional<Clock::duration> budget = std::nullopt) {
        const auto deadline =
            budget ? std::optional<Clock::time_point>(Clock::now() + *budget)
                   : std::nullopt;
        EventLoop* prev = tls_running_loop;
        tls_running_loop = this;
        if (!tls_default_loop) tls_default_loop = this;

        while (!deadline || Clock::now() < *deadline) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                flush_due_timers_unlocked();
                if (tasks_.empty()) break;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            if (task) task();
        }
        tls_running_loop = prev;
    }

private:
    struct TimerItem {
        Clock::time_point when;
        TimerId id;
        Task task;
        Clock::duration interval;  // >0 为周期

        bool operator>(const TimerItem& o) const { return when > o.when; }
    };

    ///更新时间循环时间，将循环时间放入任务队列中并定好下次时间事件
    void flush_due_timers_unlocked() {
        const auto now = Clock::now();
        while (!timers_.empty() && timers_.top().when <= now) {
            TimerItem item = timers_.top();
            timers_.pop();
            if (cancelled_.erase(item.id) > 0) continue; //如果当前任务是要删除的任务就跳过

            if (item.interval > Clock::duration::zero()) { //如果是周期时钟事件
                Task body = std::move(item.task);
                const TimerId id = item.id;
                const Clock::duration interval = item.interval;
                tasks_.push([this, body = std::move(body), id, interval]() mutable {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (cancelled_.erase(id) > 0) return;
                    }
                    body();
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!running_) return;
                    if (cancelled_.count(id) > 0) {
                        cancelled_.erase(id);
                        return;
                    }
                    timers_.push(TimerItem{Clock::now() + interval, id, //将下次的时钟放入时钟事件队列中
                                           std::move(body), interval});
                    cv_.notify_one();
                });
            } else { //如果不是时钟循环事件
                const TimerId id = item.id;
                Task body = std::move(item.task);
                tasks_.push([this, id, body = std::move(body)]() mutable {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (cancelled_.erase(id) > 0) return;
                    }
                    body();
                });
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
    std::priority_queue<TimerItem, std::vector<TimerItem>,
                        std::greater<TimerItem>>
        timers_;
    std::unordered_set<TimerId> cancelled_;
    bool running_ = true;
    TimerId next_timer_id_ = 1;
};

/// 保证当前线程有默认 EventLoop（用作线程令牌 / 亲和）
inline EventLoop* ensure_thread_loop() {
    if (tls_default_loop) return tls_default_loop;
    static thread_local std::unique_ptr<EventLoop> owned;
    if (!owned) owned = std::make_unique<EventLoop>();
    tls_default_loop = owned.get();
    return tls_default_loop;
}

/// 仿 QCoreApplication：主线程启动时注册一次默认 loop
class CoreApplication {
public:
    CoreApplication() { loop_ = ensure_thread_loop(); }
    CoreApplication(const CoreApplication&) = delete;
    CoreApplication& operator=(const CoreApplication&) = delete;

    EventLoop* thread() const noexcept { return loop_; }
    int exec() {
        loop_->run();
        return 0;
    }

private:
    EventLoop* loop_ = nullptr;
};

// =============================================================================
// WorkerThread：托管 EventLoop + std::thread
// =============================================================================
class WorkerThread {
public:
    WorkerThread() = default;
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    ~WorkerThread() { stop(); }

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable()) return;
        loop_ = std::make_unique<EventLoop>();
        EventLoop* loop = loop_.get();
        thread_ = std::thread([loop]() {
            tls_default_loop = loop;  // 工作线程亲和 = 该 loop
            loop->run();
            tls_default_loop = nullptr;
        });
    }

    void stop() {
        std::unique_ptr<EventLoop> loop;
        std::thread th;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop = std::move(loop_);
            th = std::move(thread_);
        }
        if (loop) loop->stop();
        if (!th.joinable()) return;
        if (th.get_id() == std::this_thread::get_id()) {
            th.detach();
            return;
        }
        th.join();
    }

    EventLoop* loop() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return loop_.get();
    }

    bool isRunning() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return thread_.joinable() && loop_ && loop_->isRunning();
    }

private:
    mutable std::mutex mutex_;
    std::unique_ptr<EventLoop> loop_;
    std::thread thread_;
};

// =============================================================================
// 连接状态（Signal 与 Connection 共享）
// =============================================================================
struct ConnectionState {
    std::atomic<bool> alive{true};
    std::uint64_t id = 0;
    std::function<void()> disconnect_fn;
};

class Connection {
public:
    Connection() = default;
    explicit Connection(std::shared_ptr<ConnectionState> state)
        : state_(std::move(state)) {}

    bool connected() const {
        return state_ && state_->alive.load(std::memory_order_acquire);
    }

    void disconnect() {
        if (!state_) return;
        auto s = std::move(state_);
        if (!s->alive.exchange(false, std::memory_order_acq_rel)) return;
        if (s->disconnect_fn) s->disconnect_fn();
    }

    std::uint64_t id() const { return state_ ? state_->id : 0; }

private:
    std::shared_ptr<ConnectionState> state_;
};

/// RAII：析构时自动 disconnect
class ScopedConnection {
public:
    ScopedConnection() = default;
    explicit ScopedConnection(Connection c) : conn_(std::move(c)) {}
    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;
    ScopedConnection(ScopedConnection&& o) noexcept
        : conn_(std::move(o.conn_)) {}
    ScopedConnection& operator=(ScopedConnection&& o) noexcept {
        if (this != &o) {
            disconnect();
            conn_ = std::move(o.conn_);
        }
        return *this;
    }
    ~ScopedConnection() { disconnect(); }

    void disconnect() { conn_.disconnect(); }
    bool connected() const { return conn_.connected(); }
    Connection release() { return std::move(conn_); }

private:
    Connection conn_;
};

// =============================================================================
// Object：可绑定线程 + 入站连接追踪 + 存活令牌
// =============================================================================
class Object {
public:
    Object() : alive_(std::make_shared<char>('\0')) {
        // 构造时自动绑定当前线程默认 loop（仿 QObject 线程亲和）
        loop_.store(ensure_thread_loop(), std::memory_order_release);
    }

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    virtual ~Object() { invalidate(); }

    /// 提前失效。跨线程接收者：请在派生类析构函数第一行调用，
    /// 否则 ~Object 才失效时，派生成员可能已销毁而 Queued 槽仍可能 lock 成功。
    void invalidate() noexcept {
        bool expected = true;
        //将valid从true改为false，如果是已经失效过的直接放回
        if (!valid_.compare_exchange_strong(expected, false)) return;
        alive_.reset(); //丢弃这个指针
        std::vector<std::shared_ptr<ConnectionState>> inbound;
        {
            std::lock_guard<std::mutex> lock(inbound_mutex_);
            inbound.swap(inbound_); //在锁内将本对象连接的所有连接移动到局部变量中
        }
        for (auto& s : inbound) {
            if (!s) continue;
            if (!s->alive.exchange(false, std::memory_order_acq_rel)) continue;
            if (s->disconnect_fn) s->disconnect_fn();
        }
    }

    void moveToThread(EventLoop* loop) noexcept {
        loop_.store(loop, std::memory_order_release);
    }
    EventLoop* thread() const noexcept {
        return loop_.load(std::memory_order_acquire);
    }

    /// 预约删除（仅用于 new 出来的对象）。
    /// - 目标 loop 正在泵，或调用方不在亲和线程：post 到目标 loop 再 delete
    /// - 亲和线程且当前未在泵：立即 delete（避免无 exec 时泄漏）
    void deleteLater() {
        EventLoop* loop = thread(); //获取当前对象绑定的线程
        if (!loop) { //如果当前对象没有绑定线程，则立即删除
            delete this;
            return;
        }
        //如果当前对象绑定的线程正在泵，或者调用方不在亲和线程，则将当前对象post到目标线程再删除
        if (tls_running_loop == loop || current_thread_loop() != loop) {
            loop->post([this] { delete this; });
            return;
        }
        delete this;
    }

    std::weak_ptr<void> lifetime() const { return alive_; }
    bool isValid() const noexcept {
        return valid_.load(std::memory_order_acquire);
    }

    /** @brief 跟踪入站连接,将连接状态添加到inbound_中 */
    void trackInbound(const std::shared_ptr<ConnectionState>& state) {
        std::lock_guard<std::mutex> lock(inbound_mutex_);
        inbound_.push_back(state);
    }

    /** @brief 取消跟踪入站连接,将连接状态从inbound_中移除 */
    void untrackInbound(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(inbound_mutex_);
        inbound_.erase(
            std::remove_if(inbound_.begin(), inbound_.end(),
                           [id](const std::shared_ptr<ConnectionState>& s) {
                               return !s || s->id == id;
                           }),
            inbound_.end());
    }

private:
    std::atomic<EventLoop*> loop_{nullptr};
    std::shared_ptr<void> alive_;
    std::atomic<bool> valid_{true};
    mutable std::mutex inbound_mutex_;
    std::vector<std::shared_ptr<ConnectionState>> inbound_;
};

// =============================================================================
// Object 智能指针：堆对象所有权约定（connect 仅观察，不因连接延长寿命）
// =============================================================================
struct ObjectDeleteLater {
    void operator()(Object* p) const noexcept {
        if (p) p->deleteLater();
    }
};

template <typename T>
using object_uptr = std::unique_ptr<T, ObjectDeleteLater>;

template <typename T>
using object_sptr = std::shared_ptr<T>;

template <typename T>
using object_wptr = std::weak_ptr<T>;

template <typename T, typename... Args>
object_uptr<T> make_object(Args&&... args) {
    static_assert(std::is_base_of_v<Object, T>,
                  "T must derive from qto::Object");
    return object_uptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T, typename... Args>
object_sptr<T> make_object_shared(Args&&... args) {
    static_assert(std::is_base_of_v<Object, T>,
                  "T must derive from qto::Object");
    return object_sptr<T>(new T(std::forward<Args>(args)...),
                          ObjectDeleteLater{});
}

template <typename T>
T* object_get(T* p) noexcept {
    return p;
}
template <typename T, typename D>
T* object_get(const std::unique_ptr<T, D>& p) noexcept {
    return p.get();
}
template <typename T>
T* object_get(const std::shared_ptr<T>& p) noexcept {
    return p.get();
}

// =============================================================================
// Signal
// =============================================================================
template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    ~Signal() { disconnectAll(); }

    /// 绑定到 receiver：支持 Auto/Queued/Direct；receiver 析构自动断开
    Connection connect(Object* receiver, Slot slot,
                       ConnectionType type = ConnectionType::Auto) {
        if (!slot) return {};
        return add_connection(receiver, std::move(slot), type);
    }

    /// 语法糖：成员函数槽 connect(recv, &Recv::method)
    template <typename Recv, typename SlotClass, typename... SlotArgs>
    Connection connect(Recv* receiver, void (SlotClass::*method)(SlotArgs...),
                       ConnectionType type = ConnectionType::Auto) {
        static_assert(std::is_base_of_v<Object, Recv>,
                      "receiver must derive from qto::Object");
        if (!receiver || !method) return {};
        return connect(
            static_cast<Object*>(receiver),
            [receiver, method](const Args&... args) {
                (receiver->*method)(args...);
            },
            type);
    }

    template <typename Recv, typename SlotClass, typename... SlotArgs>
    Connection connect(Recv* receiver,
                       void (SlotClass::*method)(SlotArgs...) const,
                       ConnectionType type = ConnectionType::Auto) {
        static_assert(std::is_base_of_v<Object, Recv>,
                      "receiver must derive from qto::Object");
        if (!receiver || !method) return {};
        return connect(
            static_cast<Object*>(receiver),
            [receiver, method](const Args&... args) {
                (receiver->*method)(args...);
            },
            type);
    }

    /// 智能指针语法糖（非拥有：不因连接持有 shared 延长寿命）
    template <typename Recv, typename D, typename SlotClass,
              typename... SlotArgs>
    Connection connect(const std::unique_ptr<Recv, D>& receiver,
                       void (SlotClass::*method)(SlotArgs...),
                       ConnectionType type = ConnectionType::Auto) {
        return connect(receiver.get(), method, type);
    }
    template <typename Recv, typename D, typename SlotClass,
              typename... SlotArgs>
    Connection connect(const std::unique_ptr<Recv, D>& receiver,
                       void (SlotClass::*method)(SlotArgs...) const,
                       ConnectionType type = ConnectionType::Auto) {
        return connect(receiver.get(), method, type);
    }
    template <typename Recv, typename SlotClass, typename... SlotArgs>
    Connection connect(const std::shared_ptr<Recv>& receiver,
                       void (SlotClass::*method)(SlotArgs...),
                       ConnectionType type = ConnectionType::Auto) {
        return connect(receiver.get(), method, type);
    }
    template <typename Recv, typename SlotClass, typename... SlotArgs>
    Connection connect(const std::shared_ptr<Recv>& receiver,
                       void (SlotClass::*method)(SlotArgs...) const,
                       ConnectionType type = ConnectionType::Auto) {
        return connect(receiver.get(), method, type);
    }
    template <typename Recv, typename SlotClass, typename... SlotArgs>
    Connection connect(const std::weak_ptr<Recv>& receiver,
                       void (SlotClass::*method)(SlotArgs...),
                       ConnectionType type = ConnectionType::Auto) {
        auto locked = receiver.lock();
        return connect(locked.get(), method, type);
    }
    template <typename Recv, typename SlotClass, typename... SlotArgs>
    Connection connect(const std::weak_ptr<Recv>& receiver,
                       void (SlotClass::*method)(SlotArgs...) const,
                       ConnectionType type = ConnectionType::Auto) {
        auto locked = receiver.lock();
        return connect(locked.get(), method, type);
    }
    template <typename Recv, typename D>
    Connection connect(const std::unique_ptr<Recv, D>& receiver, Slot slot,
                       ConnectionType type = ConnectionType::Auto) {
        return connect(static_cast<Object*>(receiver.get()), std::move(slot),
                       type);
    }
    template <typename Recv>
    Connection connect(const std::shared_ptr<Recv>& receiver, Slot slot,
                       ConnectionType type = ConnectionType::Auto) {
        return connect(static_cast<Object*>(receiver.get()), std::move(slot),
                       type);
    }
    template <typename Recv>
    Connection connect(const std::weak_ptr<Recv>& receiver, Slot slot,
                       ConnectionType type = ConnectionType::Auto) {
        auto locked = receiver.lock();
        return connect(static_cast<Object*>(locked.get()), std::move(slot),
                       type);
    }

    /// 无 receiver：始终在 emit 线程 Direct 调用（注意捕获对象生命周期）
    Connection connect(Slot slot) {
        if (!slot) return {};
        return add_connection(nullptr, std::move(slot), ConnectionType::Direct);
    }

    void disconnect(Connection& c) { c.disconnect(); }

    void disconnectAll() {
        std::vector<std::shared_ptr<ConnectionState>> states;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& e : entries_) {
                if (e.state) states.push_back(e.state); //如果连接状态不为空，则添加到states中
            }
            entries_.clear(); //清空entries_
        }
        for (auto& s : states) {
            if (!s) continue;
            if (!s->alive.exchange(false, std::memory_order_acq_rel)) continue;
            // disconnect_fn 会再次抢 mutex_ 移除（已空）并 untrack
            if (s->disconnect_fn) s->disconnect_fn(); //如果连接状态的disconnect_fn不为空，则调用它
        }
    }

    void emit(const Args&... args) const {
        std::vector<Entry> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.reserve(entries_.size());
            for (const auto& e : entries_) {
                if (e.state && e.state->alive.load(std::memory_order_acquire)) {
                    snapshot.push_back(e); //如果连接状态不为空，则添加到snapshot中
                }
            }
        }

        for (const auto& e : snapshot) {
            if (!e.state->alive.load(std::memory_order_acquire)) continue;

            EventLoop* target_loop = nullptr;
            if (e.receiver) {
                auto gate = e.receiver_alive.lock();
                if (!gate) continue;
                target_loop = e.receiver->thread();
            } else if (e.receiver_alive.expired()) {
                continue;
            }

            const bool same_thread = (target_loop == current_thread_loop());

            // Direct 跨线程 → 降级 Queued；Queued 无 loop → 丢弃（不静默 Direct）
            bool use_direct = false;
            if (e.type == ConnectionType::Queued) {
                if (!target_loop) continue;
                use_direct = false;
            } else if (e.type == ConnectionType::Direct) {
                if (same_thread || !target_loop) {
                    use_direct = true;
                } else {
                    use_direct = false;  // 跨线程 Direct 降级
                }
            } else {  // Auto
                use_direct = same_thread || !target_loop;
            }

            if (use_direct) {
                if (e.receiver && e.receiver_alive.expired()) continue;
                e.slot(args...);
            } else {
                if (!target_loop) continue;
                auto bound_slot = e.slot;
                auto bound_args = std::make_tuple(args...);
                auto weak = e.receiver_alive;
                target_loop->post(
                    [bound_slot = std::move(bound_slot),
                     bound_args = std::move(bound_args),
                     weak = std::move(weak)]() mutable {
                        if (!weak.lock()) return;
                        std::apply(bound_slot, std::move(bound_args));
                    });
            }
        }
    }

    void operator()(const Args&... args) const { emit(args...); }

private:
    struct Entry {
        std::shared_ptr<ConnectionState> state;
        Object* receiver = nullptr; //接收信号的object对象
        std::weak_ptr<void> receiver_alive; //接收信号的object对象是否存活
        Slot slot;
        ConnectionType type = ConnectionType::Auto;
    };

    Connection add_connection(Object* receiver, Slot slot,
                              ConnectionType type) {
        auto state = std::make_shared<ConnectionState>();
        std::uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = next_id_++;
            state->id = id;

            Entry e;
            e.state = state;
            e.receiver = receiver;
            // 无 receiver：用永不过期哨兵，避免 weak_ptr{} 的 expired()==true
            e.receiver_alive = receiver ? receiver->lifetime()
                                        : std::weak_ptr<void>(forever_);
            e.slot = std::move(slot);
            e.type = type;
            entries_.push_back(std::move(e));
        }

        state->disconnect_fn = [this, id, receiver]() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                entries_.erase(
                    std::remove_if(entries_.begin(), entries_.end(),
                                   [id](const Entry& e) {
                                       return e.state && e.state->id == id;
                                   }),
                    entries_.end());
            }
            if (receiver) receiver->untrackInbound(id);
        };

        if (receiver) receiver->trackInbound(state); //如果receiver不为空，则将连接状态添加到inbound_中
        return Connection{std::move(state)};
    }

    mutable std::mutex mutex_;
    std::vector<Entry> entries_; //这个信号有多少槽函数连接
    std::uint64_t next_id_ = 1;
    // 无 receiver 连接的存活哨兵
    std::shared_ptr<void> forever_ = std::make_shared<char>('\0');
};

// =============================================================================
// invoke：将函数投递到 object 所在线程
// =============================================================================
inline void invoke(Object* receiver, std::function<void()> fn,
                   ConnectionType type = ConnectionType::Auto) {
    if (!receiver || !fn) return;

    EventLoop* loop = receiver->thread();
    const bool same_thread = (loop == current_thread_loop());

    bool use_direct = false;
    if (type == ConnectionType::Queued) {
        if (!loop) return;
    } else if (type == ConnectionType::Direct) {
        // 跨线程 Direct 降级为 Queued
        use_direct = same_thread || !loop;
    } else {
        use_direct = same_thread || !loop;
    }

    if (use_direct) {
        if (!receiver->lifetime().lock()) return;
        fn();
        return;
    }
    if (!loop) return;

    auto weak = receiver->lifetime();
    loop->post([weak = std::move(weak), fn = std::move(fn)]() mutable {
        if (!weak.lock()) return;
        fn();
    });
}

template <typename T, typename D>
void invoke(const std::unique_ptr<T, D>& receiver, std::function<void()> fn,
            ConnectionType type = ConnectionType::Auto) {
    invoke(static_cast<Object*>(receiver.get()), std::move(fn), type);
}
template <typename T>
void invoke(const std::shared_ptr<T>& receiver, std::function<void()> fn,
            ConnectionType type = ConnectionType::Auto) {
    invoke(static_cast<Object*>(receiver.get()), std::move(fn), type);
}
template <typename T>
void invoke(const std::weak_ptr<T>& receiver, std::function<void()> fn,
            ConnectionType type = ConnectionType::Auto) {
    auto locked = receiver.lock();
    invoke(static_cast<Object*>(locked.get()), std::move(fn), type);
}

// =============================================================================
// 自由函数 connect
// =============================================================================
template <typename... Args, typename Recv, typename SlotClass,
          typename... SlotArgs>
Connection connect(Signal<Args...>& signal, Recv* receiver,
                   void (SlotClass::*method)(SlotArgs...),
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, method, type);
}

template <typename... Args, typename Recv, typename SlotClass,
          typename... SlotArgs>
Connection connect(Signal<Args...>& signal, Recv* receiver,
                   void (SlotClass::*method)(SlotArgs...) const,
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, method, type);
}

template <typename... Args>
Connection connect(Signal<Args...>& signal, Object* receiver,
                   typename Signal<Args...>::Slot slot,
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, std::move(slot), type);
}

template <typename... Args, typename Recv, typename D, typename SlotClass,
          typename... SlotArgs>
Connection connect(Signal<Args...>& signal,
                   const std::unique_ptr<Recv, D>& receiver,
                   void (SlotClass::*method)(SlotArgs...),
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, method, type);
}
template <typename... Args, typename Recv, typename D, typename SlotClass,
          typename... SlotArgs>
Connection connect(Signal<Args...>& signal,
                   const std::unique_ptr<Recv, D>& receiver,
                   void (SlotClass::*method)(SlotArgs...) const,
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, method, type);
}
template <typename... Args, typename Recv, typename SlotClass,
          typename... SlotArgs>
Connection connect(Signal<Args...>& signal,
                   const std::shared_ptr<Recv>& receiver,
                   void (SlotClass::*method)(SlotArgs...),
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, method, type);
}
template <typename... Args, typename Recv, typename SlotClass,
          typename... SlotArgs>
Connection connect(Signal<Args...>& signal,
                   const std::shared_ptr<Recv>& receiver,
                   void (SlotClass::*method)(SlotArgs...) const,
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, method, type);
}
template <typename... Args, typename Recv, typename SlotClass,
          typename... SlotArgs>
Connection connect(Signal<Args...>& signal, const std::weak_ptr<Recv>& receiver,
                   void (SlotClass::*method)(SlotArgs...),
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, method, type);
}
template <typename... Args, typename Recv, typename SlotClass,
          typename... SlotArgs>
Connection connect(Signal<Args...>& signal, const std::weak_ptr<Recv>& receiver,
                   void (SlotClass::*method)(SlotArgs...) const,
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, method, type);
}

template <typename... Args, typename Recv, typename D>
Connection connect(Signal<Args...>& signal,
                   const std::unique_ptr<Recv, D>& receiver,
                   typename Signal<Args...>::Slot slot,
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, std::move(slot), type);
}
template <typename... Args, typename Recv>
Connection connect(Signal<Args...>& signal,
                   const std::shared_ptr<Recv>& receiver,
                   typename Signal<Args...>::Slot slot,
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, std::move(slot), type);
}
template <typename... Args, typename Recv>
Connection connect(Signal<Args...>& signal, const std::weak_ptr<Recv>& receiver,
                   typename Signal<Args...>::Slot slot,
                   ConnectionType type = ConnectionType::Auto) {
    return signal.connect(receiver, std::move(slot), type);
}

}  // namespace qto

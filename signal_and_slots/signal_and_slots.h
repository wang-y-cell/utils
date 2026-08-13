#pragma once

/**
 * signal_and_slots — 轻量 Qt 风格信号/槽 + 事件循环（C++17 header-only）
 *
 * 日常用法：
 *   #include "signal_and_slots.h"
 *   using namespace utils;
 *
 * 能力：
 *   - connection_type: Direct / Queued / Auto
 *   - connection / scoped_connection（可断开、RAII）
 *   - object 析构自动断开入站连接，槽内校验存活，避免悬空
 *   - object::delete_later + object_uptr / object_sptr / object_wptr
 *   - connect / emit 线程安全；connect 支持裸指针与智能指针（非拥有观察）
 *   - event_loop：post / 延迟定时器 / 周期定时器 / process_events
 *   - worker_thread：一线程一循环
 *   - core_application / ensure_thread_loop：每线程默认 event_loop（仿 QThread 亲和）
 *   - connect 语法糖
 *   - invoke：把可调用对象投递到目标 object 所在线程
 *
 * 命名空间：utils（旧名 qto 仍可用别名兼容）
 *
 * 使用注意：
 *   1) 跨线程 object 销毁前先 worker_thread::stop() / 排空队列
 *   2) 派生类若可能被跨线程回调，析构函数第一行调用 invalidate()
 *       （堆对象优先 object_uptr / delete_later，可减少手动 invalidate）
 *   3) 跨线程 Direct 会自动降级为 Queued；Queued 且无 loop 则丢弃
 *   4) 禁止在工作线程里调用 worker_thread::stop() 期望 join 自己
 *   5) 主线程建议先构造 core_application（或依赖 object 内 ensure_thread_loop）
 *   6) 堆上 object 建议只用 object_uptr/object_sptr 拥有；connect 仅观察不延长寿命
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

namespace utils {

enum class connection_type {
    direct,  // 同步：在 emit 所在线程执行
    queued,  // 异步：投递到接收者所属 event_loop
    automatic     // 同线程 Direct，跨线程 Queued
};

class event_loop;
class object;

// 线程默认 loop（亲和身份，可不 run）
inline thread_local event_loop* tls_default_loop = nullptr;
// 当前正在 run/process_events 的 loop
inline thread_local event_loop* tls_running_loop = nullptr;

/// 当前线程 loop：优先返回正在泵的，否则返回默认亲和 loop
inline event_loop* current_thread_loop() noexcept {
    if (tls_running_loop) return tls_running_loop;
    return tls_default_loop;
}

// 前向声明；定义在 event_loop 类之后
inline event_loop* ensure_thread_loop();

// =============================================================================
// event_loop
// =============================================================================
class event_loop {
public:
    using clock = std::chrono::steady_clock;
    using task = std::function<void()>;
    using timer_id = std::uint64_t;

    event_loop() = default;
    event_loop(const event_loop&) = delete;
    event_loop& operator=(const event_loop&) = delete;

    ~event_loop() { stop(); }

    /** @brief 将任务添加到任务队列中 */
    void post(task task_) {
        if (!task_) return;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_running) return;
            _tasks.push(std::move(task_));
        }
        _cv.notify_one();
    }

    /// 延迟执行。delay <= 0 等价于 post。返回可用 cancel_timer 取消的 id（post 路径为 0）。
    timer_id post_delayed(clock::duration delay, task task_) {
        if (!task_) return 0;
        if (delay <= clock::duration::zero()) {
            post(std::move(task_));
            return 0;
        }
        timer_id id = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_running) return 0;
            id = _next_timer_id++;
            _timers.push(timer_item{clock::now() + delay, id, std::move(task_),
                                    clock::duration::zero()});
        }
        _cv.notify_one();
        return id;
    }

    /// 周期执行；首次在 interval 之后触发。
    timer_id post_periodic(clock::duration interval, task task_) {
        if (!task_ || interval <= clock::duration::zero()) return 0;
        timer_id id = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_running) return 0;
            id = _next_timer_id++;
            _timers.push(timer_item{clock::now() + interval, id, std::move(task_),
                                    interval});
        }
        _cv.notify_one();
        return id;
    }

    void cancel_timer(timer_id id) {
        if (id == 0) return;
        std::lock_guard<std::mutex> lock(_mutex);
        _cancelled.insert(id);
    }

    /** @brief 运行事件循环 */
    void run() {
        event_loop* prev_running = tls_running_loop;
        tls_running_loop = this;
        if (!tls_default_loop) tls_default_loop = this;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _running = true;
        }

        while (true) {
            task task_;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                for (;;) {
                    flush_due_timers_unlocked();
                    if (!_tasks.empty()) { //任务队列有任务
                        task_ = std::move(_tasks.front());
                        _tasks.pop();
                        break;
                    }
                    //任务队列没有任务了
                    if (!_running) {
                        tls_running_loop = prev_running;
                        return;
                    }
                    if (!_timers.empty()) { //没有任务，但是有定时任务，等待
                        _cv.wait_until(lock, _timers.top()._when);
                    } else {
                        _cv.wait(lock); //任务队列没有任务，也没有定时任务
                    }
                }
            }
            if (task_) task_(); //执行任务
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _running = false;
        }
        _cv.notify_all();
    }

    bool is_running() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _running;
    }

    /**  
     * @brief 处理已到期的定时器与已排队任务；可选最长等待。会临时设置 tls。
     * 
     */
    void process_events(
        std::optional<clock::duration> budget = std::nullopt) {
        const auto deadline =
            budget ? std::optional<clock::time_point>(clock::now() + *budget)
                   : std::nullopt;
        event_loop* prev = tls_running_loop;
        tls_running_loop = this;
        if (!tls_default_loop) tls_default_loop = this;

        while (!deadline || clock::now() < *deadline) {
            task task_;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                flush_due_timers_unlocked();
                if (_tasks.empty()) break;
                task_ = std::move(_tasks.front());
                _tasks.pop();
            }
            if (task_) task_();
        }
        tls_running_loop = prev;
    }

private:
    struct timer_item {
        clock::time_point _when;
        timer_id _id;
        task _task;
        clock::duration _interval;  // >0 为周期

        bool operator>(const timer_item& o) const { return _when > o._when; }
    };

    ///更新时间循环时间，将循环时间放入任务队列中并定好下次时间事件
    void flush_due_timers_unlocked() {
        const auto now = clock::now();
        while (!_timers.empty() && _timers.top()._when <= now) {
            timer_item item = _timers.top();
            _timers.pop();
            if (_cancelled.erase(item._id) > 0) continue; //如果当前任务是要删除的任务就跳过

            if (item._interval > clock::duration::zero()) { //如果是周期时钟事件
                task body = std::move(item._task);
                const timer_id id = item._id;
                const clock::duration interval = item._interval;
                _tasks.push([this, body = std::move(body), id, interval]() mutable {
                    {
                        std::lock_guard<std::mutex> lock(_mutex);
                        if (_cancelled.erase(id) > 0) return;
                    }
                    body();
                    std::lock_guard<std::mutex> lock(_mutex);
                    if (!_running) return;
                    if (_cancelled.count(id) > 0) {
                        _cancelled.erase(id);
                        return;
                    }
                    _timers.push(timer_item{clock::now() + interval, id, //将下次的时钟放入时钟事件队列中
                                            std::move(body), interval});
                    _cv.notify_one();
                });
            } else { //如果不是时钟循环事件
                const timer_id id = item._id;
                task body = std::move(item._task);
                _tasks.push([this, id, body = std::move(body)]() mutable {
                    {
                        std::lock_guard<std::mutex> lock(_mutex);
                        if (_cancelled.erase(id) > 0) return;
                    }
                    body();
                });
            }
        }
    }

    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::queue<task> _tasks;
    std::priority_queue<timer_item, std::vector<timer_item>,
                        std::greater<timer_item>>
        _timers;
    std::unordered_set<timer_id> _cancelled;
    bool _running = true;
    timer_id _next_timer_id = 1;
};

/// 保证当前线程有默认 event_loop（用作线程令牌 / 亲和）
inline event_loop* ensure_thread_loop() {
    if (tls_default_loop) return tls_default_loop;
    static thread_local std::unique_ptr<event_loop> owned;
    if (!owned) owned = std::make_unique<event_loop>();
    tls_default_loop = owned.get();
    return tls_default_loop;
}

/// 仿 QCoreApplication：主线程启动时注册一次默认 loop
class core_application {
public:
    core_application() { _loop = ensure_thread_loop(); }
    core_application(const core_application&) = delete;
    core_application& operator=(const core_application&) = delete;

    event_loop* thread() const noexcept { return _loop; }
    int exec() {
        _loop->run();
        return 0;
    }

private:
    event_loop* _loop = nullptr;
};

// =============================================================================
// worker_thread：托管 event_loop + std::thread
// =============================================================================
class worker_thread {
public:
    worker_thread() = default;
    worker_thread(const worker_thread&) = delete;
    worker_thread& operator=(const worker_thread&) = delete;

    ~worker_thread() { stop(); }

    void start() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_thread.joinable()) return;
        _loop = std::make_unique<event_loop>();
        event_loop* loop_ = _loop.get();
        _thread = std::thread([loop_]() {
            tls_default_loop = loop_;  // 工作线程亲和 = 该 loop
            loop_->run();
            tls_default_loop = nullptr;
        });
    }

    void stop() {
        std::unique_ptr<event_loop> loop_;
        std::thread th;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            loop_ = std::move(_loop);
            th = std::move(_thread);
        }
        if (loop_) loop_->stop();
        if (!th.joinable()) return;
        if (th.get_id() == std::this_thread::get_id()) {
            th.detach();
            return;
        }
        th.join();
    }

    event_loop* loop() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _loop.get();
    }

    bool is_running() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _thread.joinable() && _loop && _loop->is_running();
    }

private:
    mutable std::mutex _mutex;
    std::unique_ptr<event_loop> _loop;
    std::thread _thread;
};

// =============================================================================
// 连接状态（signal 与 connection 共享）
// =============================================================================
struct connection_state {
    std::atomic<bool> _alive{true};
    std::uint64_t _id = 0;
    std::function<void()> _disconnect_fn;
};

class connection {
public:
    connection() = default;
    explicit connection(std::shared_ptr<connection_state> state)
        : _state(std::move(state)) {}

    bool connected() const {
        return _state && _state->_alive.load(std::memory_order_acquire);
    }

    void disconnect() {
        if (!_state) return;
        auto s = std::move(_state);
        if (!s->_alive.exchange(false, std::memory_order_acq_rel)) return;
        if (s->_disconnect_fn) s->_disconnect_fn();
    }

    std::uint64_t id() const { return _state ? _state->_id : 0; }

private:
    std::shared_ptr<connection_state> _state;
};

/// RAII：析构时自动 disconnect
class scoped_connection {
public:
    scoped_connection() = default;
    explicit scoped_connection(connection c) : _conn(std::move(c)) {}
    scoped_connection(const scoped_connection&) = delete;
    scoped_connection& operator=(const scoped_connection&) = delete;
    scoped_connection(scoped_connection&& o) noexcept
        : _conn(std::move(o._conn)) {}
    scoped_connection& operator=(scoped_connection&& o) noexcept {
        if (this != &o) {
            disconnect();
            _conn = std::move(o._conn);
        }
        return *this;
    }
    ~scoped_connection() { disconnect(); }

    void disconnect() { _conn.disconnect(); }
    bool connected() const { return _conn.connected(); }
    connection release() { return std::move(_conn); }

private:
    connection _conn;
};

// =============================================================================
// object：可绑定线程 + 入站连接追踪 + 存活令牌
// =============================================================================
class object {
public:
    object() : _alive(std::make_shared<char>('\0')) {
        // 构造时自动绑定当前线程默认 loop（仿 QObject 线程亲和）
        _loop.store(ensure_thread_loop(), std::memory_order_release);
    }

    object(const object&) = delete;
    object& operator=(const object&) = delete;

    virtual ~object() { invalidate(); }

    /// 提前失效。跨线程接收者：请在派生类析构函数第一行调用，
    /// 否则 ~object 才失效时，派生成员可能已销毁而 Queued 槽仍可能 lock 成功。
    void invalidate() noexcept {
        bool expected = true;
        //将valid从true改为false，如果是已经失效过的直接放回
        if (!_valid.compare_exchange_strong(expected, false)) return;
        _alive.reset(); //丢弃这个指针
        std::vector<std::shared_ptr<connection_state>> inbound;
        {
            std::lock_guard<std::mutex> lock(_inbound_mutex);
            inbound.swap(_inbound); //在锁内将本对象连接的所有连接移动到局部变量中
        }
        for (auto& s : inbound) {
            if (!s) continue;
            if (!s->_alive.exchange(false, std::memory_order_acq_rel)) continue;
            if (s->_disconnect_fn) s->_disconnect_fn();
        }
    }

    void move_to_thread(event_loop* loop_) noexcept {
        _loop.store(loop_, std::memory_order_release);
    }
    void move_to_thread(worker_thread* thread_) noexcept {
        move_to_thread(thread_ ? thread_->loop() : nullptr);
    }
    void move_to_thread(worker_thread& thread_) noexcept {
        move_to_thread(&thread_);
    }
    event_loop* thread() const noexcept {
        return _loop.load(std::memory_order_acquire);
    }

    /// 预约删除（仅用于 new 出来的对象）。
    /// - 目标 loop 正在泵，或调用方不在亲和线程：post 到目标 loop 再 delete
    /// - 亲和线程且当前未在泵：立即 delete（避免无 exec 时泄漏）
    void delete_later() {
        event_loop* loop_ = thread(); //获取当前对象绑定的线程
        if (!loop_) { //如果当前对象没有绑定线程，则立即删除
            delete this;
            return;
        }
        //如果当前对象绑定的线程正在泵，或者调用方不在亲和线程，则将当前对象post到目标线程再删除
        if (tls_running_loop == loop_ || current_thread_loop() != loop_) {
            loop_->post([this] { delete this; });
            return;
        }
        delete this;
    }

    std::weak_ptr<void> lifetime() const { return _alive; }
    bool is_valid() const noexcept {
        return _valid.load(std::memory_order_acquire);
    }

    /** @brief 跟踪入站连接,将连接状态添加到_inbound中 */
    void track_inbound(const std::shared_ptr<connection_state>& state) {
        std::lock_guard<std::mutex> lock(_inbound_mutex);
        _inbound.push_back(state);
    }

    /** @brief 取消跟踪入站连接,将连接状态从_inbound中移除 */
    void untrack_inbound(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(_inbound_mutex);
        _inbound.erase(
            std::remove_if(_inbound.begin(), _inbound.end(),
                           [id](const std::shared_ptr<connection_state>& s) {
                               return !s || s->_id == id;
                           }),
            _inbound.end());
    }

private:
    std::atomic<event_loop*> _loop{nullptr};
    std::shared_ptr<void> _alive;
    std::atomic<bool> _valid{true};
    mutable std::mutex _inbound_mutex;
    std::vector<std::shared_ptr<connection_state>> _inbound;
};

// =============================================================================
// object 智能指针：堆对象所有权约定（connect 仅观察，不因连接延长寿命）
// =============================================================================
struct object_delete_later {
    void operator()(object* p) const noexcept {
        if (p) p->delete_later();
    }
};

template <typename T>
using object_uptr = std::unique_ptr<T, object_delete_later>;

template <typename T>
using object_sptr = std::shared_ptr<T>;

template <typename T>
using object_wptr = std::weak_ptr<T>;

template <typename T, typename... Args>
object_uptr<T> make_object_unique(Args&&... args) {
    static_assert(std::is_base_of_v<object, T>,
                  "T must derive from utils::object");
    return object_uptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T, typename... Args>
object_sptr<T> make_object_shared(Args&&... args) {
    static_assert(std::is_base_of_v<object, T>,
                  "T must derive from utils::object");
    return object_sptr<T>(new T(std::forward<Args>(args)...),
                          object_delete_later{});
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
// signal
// =============================================================================
template <typename... Args>
class signal {
public:
    using slot = std::function<void(Args...)>;

    signal() = default;
    signal(const signal&) = delete;
    signal& operator=(const signal&) = delete;

    ~signal() { disconnect_all(); }

    /// 绑定到 receiver：支持 Auto/Queued/Direct；receiver 析构自动断开
    connection connect(object* receiver, slot slot_,
                       connection_type type = connection_type::automatic) {
        if (!slot_) return {};
        return add_connection(receiver, std::move(slot_), type);
    }

    /// 语法糖：成员函数槽 connect(recv, &recv::method)
    template <typename recv, typename slot_class, typename... slot_args>
    connection connect(recv* receiver, void (slot_class::*method)(slot_args...),
                       connection_type type = connection_type::automatic) {
        static_assert(std::is_base_of_v<object, recv>,
                      "receiver must derive from utils::object");
        if (!receiver || !method) return {};
        return connect(
            static_cast<object*>(receiver),
            [receiver, method](const Args&... args) {
                (receiver->*method)(args...);
            },
            type);
    }

    template <typename recv, typename slot_class, typename... slot_args>
    connection connect(recv* receiver,
                       void (slot_class::*method)(slot_args...) const,
                       connection_type type = connection_type::automatic) {
        static_assert(std::is_base_of_v<object, recv>,
                      "receiver must derive from utils::object");
        if (!receiver || !method) return {};
        return connect(
            static_cast<object*>(receiver),
            [receiver, method](const Args&... args) {
                (receiver->*method)(args...);
            },
            type);
    }

    /// 智能指针语法糖（非拥有：不因连接持有 shared 延长寿命）
    template <typename recv, typename D, typename slot_class,
              typename... slot_args>
    connection connect(const std::unique_ptr<recv, D>& receiver,
                       void (slot_class::*method)(slot_args...),
                       connection_type type = connection_type::automatic) {
        return connect(receiver.get(), method, type);
    }
    template <typename recv, typename D, typename slot_class,
              typename... slot_args>
    connection connect(const std::unique_ptr<recv, D>& receiver,
                       void (slot_class::*method)(slot_args...) const,
                       connection_type type = connection_type::automatic) {
        return connect(receiver.get(), method, type);
    }
    template <typename recv, typename slot_class, typename... slot_args>
    connection connect(const std::shared_ptr<recv>& receiver,
                       void (slot_class::*method)(slot_args...),
                       connection_type type = connection_type::automatic) {
        return connect(receiver.get(), method, type);
    }
    template <typename recv, typename slot_class, typename... slot_args>
    connection connect(const std::shared_ptr<recv>& receiver,
                       void (slot_class::*method)(slot_args...) const,
                       connection_type type = connection_type::automatic) {
        return connect(receiver.get(), method, type);
    }
    template <typename recv, typename slot_class, typename... slot_args>
    connection connect(const std::weak_ptr<recv>& receiver,
                       void (slot_class::*method)(slot_args...),
                       connection_type type = connection_type::automatic) {
        auto locked = receiver.lock();
        return connect(locked.get(), method, type);
    }
    template <typename recv, typename slot_class, typename... slot_args>
    connection connect(const std::weak_ptr<recv>& receiver,
                       void (slot_class::*method)(slot_args...) const,
                       connection_type type = connection_type::automatic) {
        auto locked = receiver.lock();
        return connect(locked.get(), method, type);
    }
    template <typename recv, typename D>
    connection connect(const std::unique_ptr<recv, D>& receiver, slot slot_,
                       connection_type type = connection_type::automatic) {
        return connect(static_cast<object*>(receiver.get()), std::move(slot_),
                       type);
    }
    template <typename recv>
    connection connect(const std::shared_ptr<recv>& receiver, slot slot_,
                       connection_type type = connection_type::automatic) {
        return connect(static_cast<object*>(receiver.get()), std::move(slot_),
                       type);
    }
    template <typename recv>
    connection connect(const std::weak_ptr<recv>& receiver, slot slot_,
                       connection_type type = connection_type::automatic) {
        auto locked = receiver.lock();
        return connect(static_cast<object*>(locked.get()), std::move(slot_),
                       type);
    }

    /// 无 receiver：始终在 emit 线程 Direct 调用（注意捕获对象生命周期）
    connection connect(slot slot_) {
        if (!slot_) return {};
        return add_connection(nullptr, std::move(slot_), connection_type::direct);
    }

    void disconnect(connection& c) { c.disconnect(); }

    void disconnect_all() {
        std::vector<std::shared_ptr<connection_state>> states;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (auto& e : _entries) {
                if (e._state) states.push_back(e._state); //如果连接状态不为空，则添加到states中
            }
            _entries.clear(); //清空_entries
        }
        for (auto& s : states) {
            if (!s) continue;
            if (!s->_alive.exchange(false, std::memory_order_acq_rel)) continue;
            // _disconnect_fn 会再次抢 _mutex 移除（已空）并 untrack
            if (s->_disconnect_fn) s->_disconnect_fn(); //如果连接状态的_disconnect_fn不为空，则调用它
        }
    }

    void emit(const Args&... args) const {
        // 复用 thread_local 容量，避免每次 emit 堆分配；swap 以支持同线程重入 emit
        thread_local std::vector<entry> tls_scratch;
        std::vector<entry> snapshot;
        snapshot.swap(tls_scratch);
        snapshot.clear();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            snapshot.reserve(_entries.size());
            for (const auto& e : _entries) {
                if (e._state && e._state->_alive.load(std::memory_order_acquire)) {
                    snapshot.push_back(e); //如果连接状态不为空，则添加到snapshot中
                }
            }
        }

        for (const auto& e : snapshot) {
            if (!e._state->_alive.load(std::memory_order_acquire)) continue;

            event_loop* target_loop = nullptr;
            if (e._receiver) {
                auto gate = e._receiver_alive.lock();
                if (!gate) continue;
                target_loop = e._receiver->thread();
            } else if (e._receiver_alive.expired()) {
                continue;
            }

            const bool same_thread = (target_loop == current_thread_loop());

            // Direct 跨线程 → 降级 Queued；Queued 无 loop → 丢弃（不静默 Direct）
            bool use_direct = false;
            if (e._type == connection_type::queued) {
                if (!target_loop) continue;
                use_direct = false;
            } else if (e._type == connection_type::direct) {
                if (same_thread || !target_loop) {
                    use_direct = true;
                } else {
                    use_direct = false;  // 跨线程 Direct 降级
                }
            } else {  // Auto
                use_direct = same_thread || !target_loop;
            }

            if (use_direct) {
                if (e._receiver && e._receiver_alive.expired()) continue;
                e._slot(args...);
            } else {
                if (!target_loop) continue;
                auto bound_slot = e._slot;
                auto bound_args = std::make_tuple(args...);
                auto weak = e._receiver_alive;
                target_loop->post(
                    [bound_slot = std::move(bound_slot),
                     bound_args = std::move(bound_args),
                     weak = std::move(weak)]() mutable {
                        if (!weak.lock()) return;
                        std::apply(bound_slot, std::move(bound_args));
                    });
            }
        }

        snapshot.clear();
        tls_scratch.swap(snapshot);  // 归还容量供后续 emit 复用
    }

    void operator()(const Args&... args) const { emit(args...); }

private:
    struct entry {
        std::shared_ptr<connection_state> _state;
        object* _receiver = nullptr; //接收信号的object对象
        std::weak_ptr<void> _receiver_alive; //接收信号的object对象是否存活
        slot _slot;
        connection_type _type = connection_type::automatic;
    };

    connection add_connection(object* receiver, slot slot_,
                              connection_type type) {
        auto state = std::make_shared<connection_state>();
        std::uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            id = _next_id++;
            state->_id = id;

            entry e;
            e._state = state;
            e._receiver = receiver;
            // 无 receiver：用永不过期哨兵，避免 weak_ptr{} 的 expired()==true
            e._receiver_alive = receiver ? receiver->lifetime()
                                         : std::weak_ptr<void>(_forever);
            e._slot = std::move(slot_);
            e._type = type;
            _entries.push_back(std::move(e));
        }

        state->_disconnect_fn = [this, id, receiver]() {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _entries.erase(
                    std::remove_if(_entries.begin(), _entries.end(),
                                   [id](const entry& e) {
                                       return e._state && e._state->_id == id;
                                   }),
                    _entries.end());
            }
            if (receiver) receiver->untrack_inbound(id);
        };

        if (receiver) receiver->track_inbound(state); //如果receiver不为空，则将连接状态添加到_inbound中
        return connection{std::move(state)};
    }

    mutable std::mutex _mutex;
    std::vector<entry> _entries; //这个信号有多少槽函数连接
    std::uint64_t _next_id = 1;
    // 无 receiver 连接的存活哨兵
    std::shared_ptr<void> _forever = std::make_shared<char>('\0');
};

// =============================================================================
// invoke：将函数投递到 object 所在线程
// =============================================================================
inline void invoke(object* receiver, std::function<void()> fn,
                   connection_type type = connection_type::automatic) {
    if (!receiver || !fn) return;

    event_loop* loop_ = receiver->thread();
    const bool same_thread = (loop_ == current_thread_loop());

    bool use_direct = false;
    if (type == connection_type::queued) {
        if (!loop_) return;
    } else if (type == connection_type::direct) {
        // 跨线程 Direct 降级为 Queued
        use_direct = same_thread || !loop_;
    } else {
        use_direct = same_thread || !loop_;
    }

    if (use_direct) {
        if (!receiver->lifetime().lock()) return;
        fn();
        return;
    }
    if (!loop_) return;

    auto weak = receiver->lifetime();
    loop_->post([weak = std::move(weak), fn = std::move(fn)]() mutable {
        if (!weak.lock()) return;
        fn();
    });
}

template <typename T, typename D>
void invoke(const std::unique_ptr<T, D>& receiver, std::function<void()> fn,
            connection_type type = connection_type::automatic) {
    invoke(static_cast<object*>(receiver.get()), std::move(fn), type);
}
template <typename T>
void invoke(const std::shared_ptr<T>& receiver, std::function<void()> fn,
            connection_type type = connection_type::automatic) {
    invoke(static_cast<object*>(receiver.get()), std::move(fn), type);
}
template <typename T>
void invoke(const std::weak_ptr<T>& receiver, std::function<void()> fn,
            connection_type type = connection_type::automatic) {
    auto locked = receiver.lock();
    invoke(static_cast<object*>(locked.get()), std::move(fn), type);
}

// =============================================================================
// 自由函数 connect
// =============================================================================
template <typename... Args, typename recv, typename slot_class,
          typename... slot_args>
connection connect(signal<Args...>& signal_, recv* receiver,
                   void (slot_class::*method)(slot_args...),
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, method, type);
}

template <typename... Args, typename recv, typename slot_class,
          typename... slot_args>
connection connect(signal<Args...>& signal_, recv* receiver,
                   void (slot_class::*method)(slot_args...) const,
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, method, type);
}

template <typename... Args>
connection connect(signal<Args...>& signal_, object* receiver,
                   typename signal<Args...>::slot slot_,
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, std::move(slot_), type);
}

template <typename... Args, typename recv, typename D, typename slot_class,
          typename... slot_args>
connection connect(signal<Args...>& signal_,
                   const std::unique_ptr<recv, D>& receiver,
                   void (slot_class::*method)(slot_args...),
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, method, type);
}
template <typename... Args, typename recv, typename D, typename slot_class,
          typename... slot_args>
connection connect(signal<Args...>& signal_,
                   const std::unique_ptr<recv, D>& receiver,
                   void (slot_class::*method)(slot_args...) const,
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, method, type);
}
template <typename... Args, typename recv, typename slot_class,
          typename... slot_args>
connection connect(signal<Args...>& signal_,
                   const std::shared_ptr<recv>& receiver,
                   void (slot_class::*method)(slot_args...),
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, method, type);
}
template <typename... Args, typename recv, typename slot_class,
          typename... slot_args>
connection connect(signal<Args...>& signal_,
                   const std::shared_ptr<recv>& receiver,
                   void (slot_class::*method)(slot_args...) const,
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, method, type);
}
template <typename... Args, typename recv, typename slot_class,
          typename... slot_args>
connection connect(signal<Args...>& signal_, const std::weak_ptr<recv>& receiver,
                   void (slot_class::*method)(slot_args...),
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, method, type);
}
template <typename... Args, typename recv, typename slot_class,
          typename... slot_args>
connection connect(signal<Args...>& signal_, const std::weak_ptr<recv>& receiver,
                   void (slot_class::*method)(slot_args...) const,
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, method, type);
}

template <typename... Args, typename recv, typename D>
connection connect(signal<Args...>& signal_,
                   const std::unique_ptr<recv, D>& receiver,
                   typename signal<Args...>::slot slot_,
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, std::move(slot_), type);
}
template <typename... Args, typename recv>
connection connect(signal<Args...>& signal_,
                   const std::shared_ptr<recv>& receiver,
                   typename signal<Args...>::slot slot_,
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, std::move(slot_), type);
}
template <typename... Args, typename recv>
connection connect(signal<Args...>& signal_, const std::weak_ptr<recv>& receiver,
                   typename signal<Args...>::slot slot_,
                   connection_type type = connection_type::automatic) {
    return signal_.connect(receiver, std::move(slot_), type);
}

}  // namespace utils
  

#pragma once

/**
 * signal_and_slots — 轻量 Qt 风格信号/槽 + 事件循环（C++20 header-only）
 *
 * 日常用法：
 *   #include "signal_and_slots.h"
 *   using namespace utils;
 *
 * 能力：
 *   - connection_type: Direct / Queued / BlockingQueued / Auto
 *   - 成员槽必须返回 slots_t / slots_t<T>；也可 connect(receiver,
 *     lambda)（不要求 slots_t）
 *   - unique 连接：同一接收者 + 同一成员槽只连一次（lambda 不做 unique）
 *   - object::block_signals：批量改状态时暂停该对象发出的信号（信号需
 *     signal{this}）
 *   - signal::disconnect(receiver)：按接收者断开
 *   - connection / scoped_connection（可断开、RAII）
 *   - object 析构自动断开入站连接；Queued 槽执行前再查 is_valid()；invalidate
 *     同亲和排空
 *   - object::delete_later + object_uptr / object_sptr / object_wptr
 *   - connect / emit 线程安全；connect 支持裸指针与智能指针（非拥有观察）
 *   - emit(Args...) 按值入参（decay-copy）；Queued 再打包进队列
 *   - event_loop：post / 延迟定时器 / 周期定时器 / process_events（budget
 *     内可候定时器）
 *   - thread / current_thread / worker_thread：线程亲和句柄（仿 QThread）
 *     current_thread：本线程 run；worker_thread：新线程 run；均含 start/stop/loop
 *   - core_application：主线程注册 ensure_thread()；exec() → thread::start()
 *   - connect 语法糖；emit / connect 丢弃槽返回值
 *   - invoke(槽)：Direct / BlockingQueued 用 result<T> 取回 slots_t 中的值
 *   - invoke(object*, type, fn, args...)：在目标线程调用普通函数/lambda，result
 *     取返回值
 *   - invoke(object*, function<void()>)：只投递无返回值任务（不取 result）
 *
 * 命名空间：utils
 *
 * 使用注意：
 *   1) 跨线程 object 销毁前先 worker_thread::stop() / 排空队列
 *   2) 派生类析构第一行必须 invalidate()（会断连；若当前在亲和线程则
 *      process_events 排空）（堆对象优先 object_uptr / delete_later，可减少手动
 *      invalidate）
 *   3) 跨线程 Direct 会自动降级为 Queued；Queued/BlockingQueued 无 loop 则丢弃
 *      BlockingQueued 要求目标 loop 正在泵（is_pumping）；未泵则跳过/失败，避免死等
 *      仅当「当前线程正在泵本 loop」时 post_blocking 因自死锁失败
 *      跨线程且无 loop 时 Auto/Direct 不再降级为发射线程 Direct，直接跳过
 *   4) 禁止在工作线程里调用 worker_thread::stop()（硬失败，不销毁 loop）
 *   5) 主线程建议先构造 core_application（或依赖 object 内 ensure_thread）
 *   6) 堆上 object 建议只用 object_uptr/object_sptr 拥有；connect 仅观察不延长寿命
 *   7) 亲和只绑 utils::thread*（move_to_thread）；同线程判断比较 thread*。
 *      投递用 object::loop_shared() / loop()。绑 worker 后 stop 再 Queued/Auto emit 安全跳过，再 start
 *      自动绑新 loop。不要用 event_loop* 做亲和。
 *   8) event_loop 泵送时吞掉单任务异常，避免单个 Queued 槽打穿整条循环
 */
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_set>
#include <utility>
#include <vector>

#include "reliability/result/expected.h"

namespace utils {

enum class connection_type {
	direct,          // 同步：在 emit 所在线程执行
	queued,          // 异步：投递到接收者所属 event_loop
	blocking_queued, // 跨线程投递并等待目标线程执行完成（要求目标 loop 正在 run）
	automatic        // 同线程 Direct，跨线程 Queued
};

inline constexpr bool unique_connection = true;

/**
* @brief 槽函数返回类型：标记「这是槽」，需要时带一个值。
* @note connect / emit 丢弃返回值；只有 invoke 会 get()。
*/
template <class T = void> 
class slots_t {
	T value_{};

public:
	slots_t() = default;
	slots_t(const slots_t &) = default;
	slots_t(slots_t &&) = default;
	slots_t &operator=(const slots_t &) = default;
	slots_t &operator=(slots_t &&) = default;
	operator T() const noexcept(std::is_nothrow_copy_constructible_v<T>) {
		return value_;
	}

	template <class U,
		std::enable_if_t<!std::is_same_v<std::decay_t<U>, slots_t> &&
		std::is_constructible_v<T, U>,
		int> = 0>
	slots_t(U &&v) : value_(std::forward<U>(v)) {}

	T &get() & noexcept { return value_; }
	const T &get() const & noexcept { return value_; }
	T get() && noexcept { return std::move(value_); }
};

template <> 
class slots_t<void> {
public:
	slots_t() = default;
};

class event_loop;
class object;
class thread;

/// 当前 OS 线程的 utils::thread 句柄（worker 运行时指向该 worker）
inline thread_local thread *tls_current_thread = nullptr;
/// 当前 OS 线程正在泵的 event_loop（仅本线程可见；用于 post_blocking 防自死锁）
inline thread_local event_loop *tls_pumping_loop = nullptr;

// 前向声明；定义在 event_loop / thread 类之后
inline thread *ensure_thread();

namespace detail {

/// 泵 loop 时标记本线程 tls_pumping_loop 与对象泵送深度。
struct event_loop_pump_scope {
	event_loop *self;
	event_loop *prev_pumping;
	explicit event_loop_pump_scope(event_loop *loop) noexcept;
	~event_loop_pump_scope();
	event_loop_pump_scope(const event_loop_pump_scope &) = delete;
	event_loop_pump_scope &operator=(const event_loop_pump_scope &) = delete;
};

} // namespace detail

// =============================================================================
// event_loop
// =============================================================================
class event_loop {
public:
	using clock = std::chrono::steady_clock;
	using task = std::function<void()>;
	using timer_id = std::uint64_t;

	event_loop() = default;
	event_loop(const event_loop &) = delete;
	event_loop &operator=(const event_loop &) = delete;

	~event_loop() { stop(); }

	/** @brief 将任务添加到任务队列中；未 accepting 时返回 false 并丢弃 */
	bool post(task task_) { return post_impl(std::move(task_)); }

	/**
	*@brief 将阻塞任务添加到队列中，添加成功则返回true，否则返回false
	* 此函数会阻塞，直到任务执行完成
	*@param task_ 阻塞任务
	*@return 添加成功则返回true，否则返回false
	*/
	bool post_blocking(task task_) {
		if (!task_)
			return false;
		// 仅当「当前线程」正在泵「本」loop 时拒绝（防自死锁）。
		if (tls_pumping_loop == this)
			return false;
		// 目标无人泵送时阻塞等待会死等（如主线程未 exec）；直接失败。
		if (!is_pumping())
			return false;
		auto state = std::make_shared<blocking_post_state>();
		if (!post_impl([task_ = std::move(task_), state]() mutable {
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					if (state->st == blocking_post_state::phase::cancelled)
						return;
					state->st = blocking_post_state::phase::running;
				}
				try {
					task_();
				} catch (...) {
					std::lock_guard<std::mutex> lock(state->mutex);
					state->error = std::current_exception();
					state->st = blocking_post_state::phase::done;
					state->cv.notify_one();
					return;
				}
				std::lock_guard<std::mutex> lock(state->mutex);
				state->st = blocking_post_state::phase::done;
				state->cv.notify_one();
			})) {
			return false;
		}
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			using phase = blocking_post_state::phase;
			while (state->st == phase::pending || state->st == phase::running) {
				if (state->cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
						return state->st == phase::done ||
							state->st == phase::cancelled;
					})) {
					break;
				}
				if (!is_pumping() && state->st == phase::pending) {
					state->st = phase::cancelled;
					state->cv.notify_one();
					return false;
				}
			}
			if (state->st == phase::cancelled)
				return false;
		}
		if (state->error)
			std::rethrow_exception(state->error);
		return true;
	}

	/// 延迟执行。delay <= 0 等价于 post。返回可用 cancel_timer 取消的 id（post
	/// 路径为 0）。
	timer_id post_delayed(clock::duration delay, task task_) {
		if (!task_)
			return 0;
		if (delay <= clock::duration::zero()) {
			post(std::move(task_));
			return 0;
		}
		timer_id id = 0;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (!_running)
				return 0;
			id = _next_timer_id++;
			_timers.push(timer_item{clock::now() + delay, id, std::move(task_),
				clock::duration::zero()});
		}
		_cv.notify_one();
		return id;
	}

	/// 周期执行；首次在 interval 之后触发。
	timer_id post_periodic(clock::duration interval, task task_) {
		if (!task_ || interval <= clock::duration::zero())
			return 0;
		timer_id id = 0;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (!_running)
				return 0;
			id = _next_timer_id++;
			_timers.push(
				timer_item{clock::now() + interval, id, std::move(task_), interval});
		}
		_cv.notify_one();
		return id;
	}

	void cancel_timer(timer_id id) {
		if (id == 0)
			return;
		std::lock_guard<std::mutex> lock(_mutex);
		_cancelled.insert(id);
	}

	/** @brief 运行事件循环。stop() 之后不会自行恢复 accepting，需 set_accepting(true) 再 run。 */
	void run() {
		detail::event_loop_pump_scope pump(this);
		std::uint64_t epoch = 0;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (!_running)
				return;
			epoch = _stop_epoch;
		}

		while (true) {
			task task_;
			{
				std::unique_lock<std::mutex> lock(_mutex);
				for (;;) {
					flush_due_timers_unlocked();
					if (!_tasks.empty()) { // 任务队列有任务
						task_ = std::move(_tasks.front());
						_tasks.pop();
						break;
					}
					if (!_running || _stop_epoch != epoch) {
						return;
					}
					if (!_timers.empty()) {
						const auto when = _timers.top()._when;
						_cv.wait_until(lock, when, [this, when, epoch] {
							return !_running || _stop_epoch != epoch ||
								!_tasks.empty() ||
								(!_timers.empty() && _timers.top()._when < when);
						});
					} else {
						_cv.wait(lock, [this, epoch] {
							return !_running || _stop_epoch != epoch ||
								!_tasks.empty() || !_timers.empty();
						});
					}
				}
			}
			if (task_) {
				try {
					task_(); // 执行任务；单任务异常不得打穿整条事件循环
				} catch (...) {
				}
			}
		}
	}

	/** @brief 停止事件循环，唤醒所有被当前loop阻塞的线程 */
	void stop() {
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_running = false;
			++_stop_epoch;
		}
		_cv.notify_all();
	}

	/** @brief 是否仍接受 post（与 is_running 相同语义） */
	bool is_running() const {
		std::lock_guard<std::mutex> lock(_mutex);
		return _running;
	}

	/// 恢复接受 post（current_thread 在本线程 start 返回后调用）
	void set_accepting(bool on) {
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_running = on;
		}
		if (on)
			_cv.notify_all();
	}

	/** @brief 当前是否正在 run()/process_events() 泵送（任意驱动线程） */
	bool is_pumping() const noexcept {
		return _pumping_depth.load(std::memory_order_acquire) > 0;
	}

	/** @brief 当前 OS 线程是否正在泵「本」loop（post_blocking 防自死锁用） */
	bool is_pumping_on_current_thread() const noexcept {
		return tls_pumping_loop == this;
	}

	/**
	* @brief 处理已到期的定时器与已排队任务；可选最长等待。
	* @note 无 budget：只排空当前到期/已排队任务，不等待未来定时器（供 invalidate
	* 排空）。 有 budget：空闲时可 wait 到下一定时器或 deadline。
	*/
	void process_events(std::optional<clock::duration> budget = std::nullopt) {
		const auto deadline =
			budget ? std::optional<clock::time_point>(clock::now() + *budget)
				: std::nullopt;
		detail::event_loop_pump_scope pump(this);

		while (!deadline || clock::now() < *deadline) {
			task task_;
			{
				std::unique_lock<std::mutex> lock(_mutex);
				flush_due_timers_unlocked();
				if (!_tasks.empty()) {
					task_ = std::move(_tasks.front());
					_tasks.pop();
				} else if (!deadline) {
					break; // 无预算：不候未来 timer
				} else if (!_timers.empty()) {
					const auto next = _timers.top()._when;
					if (next >= *deadline)
						break;
					_cv.wait_until(lock, next);
					continue;
				} else {
					break;
				}
			}
			if (task_) {
				try {
					task_();
				} catch (...) {
				}
			}
		}
	}

private:
	friend struct detail::event_loop_pump_scope;

	struct blocking_post_state {
		enum class phase { pending, running, done, cancelled };
		std::mutex mutex;
		std::condition_variable cv;
		phase st = phase::pending;
		std::exception_ptr error;
	};

	struct timer_item {
		clock::time_point _when;
		timer_id _id;
		task _task;
		clock::duration _interval; // >0 为周期

		bool operator>(const timer_item &o) const { return _when > o._when; }
	};

	/// 更新时间循环时间，将循环时间放入任务队列中并定好下次时间事件
	void flush_due_timers_unlocked() {
		const auto now = clock::now();
		while (!_timers.empty() && _timers.top()._when <= now) {
			timer_item item = _timers.top();
			_timers.pop();
			if (_cancelled.erase(item._id) > 0)
				continue; // 如果当前任务是要删除的任务就跳过

			if (item._interval > clock::duration::zero()) { // 如果是周期时钟事件
				task body = std::move(item._task);
				const timer_id id = item._id;
				const clock::duration interval = item._interval;
				_tasks.push([this, body = std::move(body), id, interval]() mutable {
					{
						std::lock_guard<std::mutex> lock(_mutex);
						if (_cancelled.erase(id) > 0)
							return;
					}
					try {
						body();
					} catch (...) {
					}
					std::lock_guard<std::mutex> lock(_mutex);
					if (!_running)
						return;
					if (_cancelled.count(id) > 0) {
						_cancelled.erase(id);
						return;
					}
					_timers.push(timer_item{clock::now() + interval,
						id, // 将下次的时钟放入时钟事件队列中
						std::move(body), interval});
					_cv.notify_one();
				});
			} else { // 如果不是时钟循环事件
				const timer_id id = item._id;
				task body = std::move(item._task);
				_tasks.push([this, id, body = std::move(body)]() mutable {
					{
						std::lock_guard<std::mutex> lock(_mutex);
						if (_cancelled.erase(id) > 0)
							return;
					}
					body();
				});
			}
		}
	}

	/** @brief 将任务添加到任务队列中 */
	bool post_impl(task task_) {
		if (!task_)
			return false;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (!_running)
				return false;
			_tasks.push(std::move(task_));
		}
		_cv.notify_one();
		return true;
	}

	mutable std::mutex _mutex;
	std::condition_variable _cv;
	std::queue<task> _tasks;
	std::priority_queue<timer_item, std::vector<timer_item>,
		std::greater<timer_item>>
	_timers;
	std::unordered_set<timer_id> _cancelled;
	bool _running = true;
	std::uint64_t _stop_epoch = 0;
	std::atomic<int> _pumping_depth{0};
	timer_id _next_timer_id = 1;
};

inline detail::event_loop_pump_scope::event_loop_pump_scope(
	event_loop *loop) noexcept
	: self(loop), prev_pumping(tls_pumping_loop) {
	if (self) {
		self->_pumping_depth.fetch_add(1, std::memory_order_release);
		tls_pumping_loop = self;
	}
}

inline detail::event_loop_pump_scope::~event_loop_pump_scope() {
	if (self)
		self->_pumping_depth.fetch_sub(1, std::memory_order_release);
	tls_pumping_loop = prev_pumping;
}

// =============================================================================
// thread：线程亲和句柄（仿 QThread）；current_thread / worker_thread
// =============================================================================
class thread {
public:
	thread(const thread &) = delete;
	thread &operator=(const thread &) = delete;
	virtual ~thread() = default;

	/// 启动事件循环：current_thread 在本线程阻塞泵；worker_thread 起新线程
	virtual void start() = 0;
	/// 停止事件循环（worker 会 join；禁止在 worker 自身线程内调用）
	virtual void stop() = 0;
	virtual bool is_running() const = 0;
	/// 投递用 event_loop；worker 已 stop 时可能为空
	virtual std::shared_ptr<event_loop> loop_shared() const = 0;
	event_loop *loop() const { return loop_shared().get(); }

	std::weak_ptr<void> identity() const noexcept { return _identity; }

protected:
	thread() = default;
	std::shared_ptr<void> _identity{std::make_shared<char>('\0')};
};

/// 代表「当前 OS 线程」的句柄（不 spawn）；拥有本线程默认 event_loop
class current_thread final : public thread {
public:
	current_thread() : _loop(std::make_shared<event_loop>()) {}

	~current_thread() override {
		if (tls_current_thread == this)
			tls_current_thread = nullptr;
	}

	void start() override {
		if (!_loop)
			return;
		_loop->run();
		// 本线程泵结束后仍接受 post，供后续 process_events / 再次 start
		_loop->set_accepting(true);
	}

	void stop() override {
		// 仅在正在泵时 stop，避免把共享的 ensure_thread loop 永久关掉
		if (_loop && _loop->is_pumping())
			_loop->stop();
	}

	std::shared_ptr<event_loop> loop_shared() const override { return _loop; }

	bool is_running() const override {
		return _loop && _loop->is_running();
	}

private:
	std::shared_ptr<event_loop> _loop;
};

/// 当前线程句柄（仿 QThread::currentThread）
inline thread *ensure_thread() {
	if (tls_current_thread)
		return tls_current_thread;
	static thread_local std::unique_ptr<current_thread> owned;
	if (!owned)
		owned = std::make_unique<current_thread>();
	tls_current_thread = owned.get();
	return tls_current_thread;
}

/// 仿 QCoreApplication：主线程注册默认 loop + 线程句柄
class core_application {
public:
	core_application() { _thread = ensure_thread(); }
	core_application(const core_application &) = delete;
	core_application &operator=(const core_application &) = delete;

	/** @brief 当前线程亲和句柄 */
	thread *thread() const noexcept { return _thread; }
	/** @brief 当前线程默认 event_loop */
	event_loop *loop() const noexcept {
		return _thread ? _thread->loop() : nullptr;
	}
	/** @brief 执行事件循环（本线程 start） */
	int exec() {
		if (_thread)
			_thread->start();
		return 0;
	}

private:
	utils::thread *_thread = nullptr;
};

// =============================================================================
// worker_thread：后台托管 event_loop + std::thread
// =============================================================================
class worker_thread final : public thread {
public:
	worker_thread() = default;
	worker_thread(const worker_thread &) = delete;
	worker_thread &operator=(const worker_thread &) = delete;

	~worker_thread() override { stop(); }

	void start() override {
		std::lock_guard<std::mutex> lock(_mutex);
		if (_std_thread.joinable())
			return;
		_loop = std::make_shared<event_loop>();
		auto loop_ = _loop;
		_std_thread = std::thread([this, loop_]() {
			tls_current_thread = this; // 本 OS 线程的 ensure_thread() → 本 worker
			loop_->run();
			tls_current_thread = nullptr;
		});
	}

	void stop() override {
		std::shared_ptr<event_loop> loop_;
		std::thread th;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			loop_ = std::move(_loop);
			th = std::move(_std_thread);
		}
		if (!loop_ && !th.joinable())
			return;
		if (th.joinable() && th.get_id() == std::this_thread::get_id()) {
			std::lock_guard<std::mutex> lock(_mutex);
			_loop = std::move(loop_);
			_std_thread = std::move(th);
			assert(false &&
				"worker_thread::stop() must not be called from its own thread");
			return;
		}
		if (loop_)
			loop_->stop();
		if (th.joinable())
			th.join();
	}

	std::shared_ptr<event_loop> loop_shared() const override {
		std::lock_guard<std::mutex> lock(_mutex);
		return _loop;
	}

	bool is_running() const override {
		std::lock_guard<std::mutex> lock(_mutex);
		return _std_thread.joinable() && _loop && _loop->is_running();
	}

private:
	mutable std::mutex _mutex;
	std::shared_ptr<event_loop> _loop;
	std::thread _std_thread;
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
		if (!_state)
			return;
		auto s = std::move(_state);
		if (!s->_alive.exchange(false, std::memory_order_acq_rel))
			return;
		if (s->_disconnect_fn)
			s->_disconnect_fn();
	}

	std::uint64_t id() const { return _state ? _state->_id : 0; }

private:
	std::shared_ptr<connection_state> _state;
};

class scoped_connection {
public:
	scoped_connection() = default;
	explicit scoped_connection(connection c) : _conn(std::move(c)) {}
	scoped_connection(const scoped_connection &) = delete;
	scoped_connection &operator=(const scoped_connection &) = delete;
	scoped_connection(scoped_connection &&o) noexcept
		: _conn(std::move(o._conn)) {}
	scoped_connection &operator=(scoped_connection &&o) noexcept {
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
// object：绑定 utils::thread + 入站连接追踪 + 存活令牌
// =============================================================================
class object {
public:
	object() : _alive(std::make_shared<char>('\0')) {
		_affinity = ensure_thread();
		_affinity_alive = _affinity->identity();
	}

	object(const object &) = delete;
	object &operator=(const object &) = delete;

	virtual ~object() { invalidate(); }

	/// 提前失效。派生类析构函数第一行必须调用。
	void invalidate() {
		bool expected = true;
		if (!_valid.compare_exchange_strong(expected, false))
			return;
		_alive.reset();
		std::vector<std::shared_ptr<connection_state>> inbound;
		{
			std::lock_guard<std::mutex> lock(_inbound_mutex);
			inbound.swap(_inbound);
		}
		for (auto &s : inbound) {
			if (!s)
				continue;
			if (!s->_alive.exchange(false, std::memory_order_acq_rel))
				continue;
			if (s->_disconnect_fn)
				s->_disconnect_fn();
		}
		auto loop_ = loop_shared();
		if (loop_ && thread() == ensure_thread()) {
			loop_->process_events();
		}
	}

	/// 绑定到线程句柄；nullptr 表示回到 ensure_thread()（当前调用方线程）
	void move_to_thread(thread *thread_) noexcept {
		if (!thread_)
			thread_ = ensure_thread();
		std::lock_guard<std::mutex> lock(_affinity_mutex);
		_affinity = thread_;
		_affinity_alive = thread_->identity();
	}
	void move_to_thread(thread &thread_) noexcept { move_to_thread(&thread_); }

	/// 当前亲和线程句柄（仿 QObject::thread）；句柄已毁时为 nullptr
	thread *thread() const noexcept {
		std::lock_guard<std::mutex> lock(_affinity_mutex);
		if (!_affinity)
			return nullptr;
		if (!_affinity_alive.lock())
			return nullptr;
		return _affinity;
	}

	/// 解析出的投递 loop；绑 worker 且已 stop / 句柄已毁时可能为空
	std::shared_ptr<event_loop> loop_shared() const noexcept {
		utils::thread *t = thread();
		return t ? t->loop_shared() : nullptr;
	}
	event_loop *loop() const noexcept { return loop_shared().get(); }

	/// 若亲和为 worker_thread 则返回之，否则 nullptr
	worker_thread *worker() const noexcept {
		return dynamic_cast<worker_thread *>(thread());
	}

	void delete_later() {
		utils::thread *aff = thread();
		auto loop_ = loop_shared();
		if (!loop_) {
			delete this;
			return;
		}
		// 不在亲和线程，或本线程正在泵亲和 loop：post 后再删
		if (ensure_thread() != aff || loop_->is_pumping_on_current_thread()) {
			if (!loop_->post([this] { delete this; }))
				delete this;
			return;
		}
		delete this;
	}

	std::weak_ptr<void> lifetime() const { return _alive; }
	bool is_valid() const noexcept {
		return _valid.load(std::memory_order_acquire);
	}

	void block_signals(bool block) noexcept {
		_signals_blocked.store(block, std::memory_order_release);
	}
	[[nodiscard]] bool signals_blocked() const noexcept {
		return _signals_blocked.load(std::memory_order_acquire);
	}

	bool track_inbound(const std::shared_ptr<connection_state> &state) {
		std::lock_guard<std::mutex> lock(_inbound_mutex);
		if (!_valid.load(std::memory_order_acquire))
			return false;
		_inbound.push_back(state);
		return true;
	}

	void untrack_inbound(std::uint64_t id) {
		std::lock_guard<std::mutex> lock(_inbound_mutex);
		_inbound.erase(
			std::remove_if(_inbound.begin(), _inbound.end(),
			[id](const std::shared_ptr<connection_state> &s) {
			return !s || s->_id == id;
		}),
			_inbound.end());
	}

private:
	mutable std::mutex _affinity_mutex;
	utils::thread *_affinity = nullptr;
	std::weak_ptr<void> _affinity_alive;
	std::shared_ptr<void> _alive;
	std::atomic<bool> _valid{true};
	std::atomic<bool> _signals_blocked{false};
	mutable std::mutex _inbound_mutex;
	std::vector<std::shared_ptr<connection_state>> _inbound;
};

// =============================================================================
// object 智能指针：堆对象所有权约定（connect 仅观察，不因连接延长寿命）
// =============================================================================
struct object_delete_later {
	void operator()(object *p) const noexcept {
		if (p)
			p->delete_later();
	}
};

template <typename T>
using object_uptr = std::unique_ptr<T, object_delete_later>;

template <typename T> using object_sptr = std::shared_ptr<T>;

template <typename T> using object_wptr = std::weak_ptr<T>;

template <typename T, typename... Args>
object_uptr<T> make_object_unique(Args &&...args) {
	static_assert(std::is_base_of_v<object, T>,
		"T must derive from utils::object");
	return object_uptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T, typename... Args>
object_sptr<T> make_object_shared(Args &&...args) {
	static_assert(std::is_base_of_v<object, T>,
		"T must derive from utils::object");
	return object_sptr<T>(new T(std::forward<Args>(args)...),
		object_delete_later{});
}

template <typename T> T *object_get(T *p) noexcept { return p; }
template <typename T, typename D>
T *object_get(const std::unique_ptr<T, D> &p) noexcept {
	return p.get();
}
template <typename T> T *object_get(const std::shared_ptr<T> &p) noexcept {
	return p.get();
}

// =============================================================================
// signal
// =============================================================================
template <typename... Args> class signal {
public:
	using slot = std::function<void(Args...)>; // 槽函数类型

	explicit signal(object *owner) noexcept
		: _owner(owner),
			_owner_alive(owner ? owner->lifetime() : std::weak_ptr<void>{}) {}
	signal(const signal &) = delete;
	signal &operator=(const signal &) = delete;

	~signal() { disconnect_all(); }

	void block_signals(bool block) noexcept {
		_blocked.store(block, std::memory_order_release);
	}

	/**
	* @brief 判断信号是否被阻塞
	* @return 如果信号被阻塞，则返回 true，否则返回 false
	*/
	[[nodiscard]] bool signals_blocked() const noexcept {
		if (_blocked.load(std::memory_order_acquire))
			return true;
		if (!_owner)
			return false;
		auto gate = _owner_alive.lock();
		if (!gate)
			return true;
		return _owner->signals_blocked();
	}

	/// 语法糖：成员函数槽 connect(recv, &recv::method)，方法必须返回 slots_t<R>
	template <typename recv, typename slot_class, typename R,
		typename... slot_args>
	connection connect(recv *receiver,
		slots_t<R> (slot_class::*method)(slot_args...),
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect_pmf(receiver, method, type, unique);
	}

	template <typename recv, typename slot_class, typename R,
		typename... slot_args>
	connection connect(recv *receiver,
		slots_t<R> (slot_class::*method)(slot_args...) const,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect_pmf(receiver, method, type, unique);
	}

	/// 绑定到 object 的可调用对象（lambda 等）；执行前校验 lifetime / is_valid
	template <typename recv, typename F,
		std::enable_if_t<
		std::is_base_of_v<object, recv> &&
		std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
		!std::is_member_function_pointer_v<std::decay_t<F>>,
		int> = 0>
	connection connect(recv *receiver, F &&func,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		(void)unique; // lambda 不做 unique 去重
		if (!receiver)
			return {};
		object *obj = static_cast<object *>(receiver);
		return add_connection(
			obj,
			[obj, fn = std::decay_t<F>(std::forward<F>(func))](
			const Args &...args) mutable {
			if (!obj->is_valid())
				return;
			(void)fn(args...);
		},
			type, false, slot_key{});
	}

	/// 智能指针语法糖（非拥有：不因连接持有 shared 延长寿命）
	template <typename recv, typename D, typename slot_class, typename R,
		typename... slot_args>
	connection connect(const std::unique_ptr<recv, D> &receiver,
		slots_t<R> (slot_class::*method)(slot_args...),
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect(receiver.get(), method, type, unique);
	}
	template <typename recv, typename D, typename slot_class, typename R,
		typename... slot_args>
	connection connect(const std::unique_ptr<recv, D> &receiver,
		slots_t<R> (slot_class::*method)(slot_args...) const,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect(receiver.get(), method, type, unique);
	}
	template <typename recv, typename D, typename F,
		std::enable_if_t<
		std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
		!std::is_member_function_pointer_v<std::decay_t<F>>,
		int> = 0>
	connection connect(const std::unique_ptr<recv, D> &receiver, F &&func,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect(receiver.get(), std::forward<F>(func), type, unique);
	}
	template <typename recv, typename slot_class, typename R,
		typename... slot_args>
	connection connect(const std::shared_ptr<recv> &receiver,
		slots_t<R> (slot_class::*method)(slot_args...),
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect(receiver.get(), method, type, unique);
	}
	template <typename recv, typename slot_class, typename R,
		typename... slot_args>
	connection connect(const std::shared_ptr<recv> &receiver,
		slots_t<R> (slot_class::*method)(slot_args...) const,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect(receiver.get(), method, type, unique);
	}
	template <typename recv, typename F,
		std::enable_if_t<
		std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
		!std::is_member_function_pointer_v<std::decay_t<F>>,
		int> = 0>
	connection connect(const std::shared_ptr<recv> &receiver, F &&func,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect(receiver.get(), std::forward<F>(func), type, unique);
	}
	template <typename recv, typename slot_class, typename R,
		typename... slot_args>
	connection connect(const std::weak_ptr<recv> &receiver,
		slots_t<R> (slot_class::*method)(slot_args...),
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked.get(), method, type, unique);
	}
	template <typename recv, typename slot_class, typename R,
		typename... slot_args>
	connection connect(const std::weak_ptr<recv> &receiver,
		slots_t<R> (slot_class::*method)(slot_args...) const,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked.get(), method, type, unique);
	}
	template <typename recv, typename F,
		std::enable_if_t<
		std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
		!std::is_member_function_pointer_v<std::decay_t<F>>,
		int> = 0>
	connection connect(const std::weak_ptr<recv> &receiver, F &&func,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked.get(), std::forward<F>(func), type, unique);
	}

	void disconnect(connection &c) { c.disconnect(); }

	/// 断开该接收者在本信号上的全部连接
	void disconnect(object *receiver) {
		if (!receiver)
			return;
		std::vector<std::shared_ptr<connection_state>> states;
		{
			std::lock_guard<std::mutex> lock(_ctl->mutex);
			for (auto &e : _ctl->entries) {
				if (e._receiver != receiver || !e._state)
					continue;
				if (e._state->_alive.exchange(false, std::memory_order_acq_rel)) {
					states.push_back(e._state);
				}
			}
			_ctl->entries.erase(std::remove_if(_ctl->entries.begin(),
				_ctl->entries.end(),
				[receiver](const entry &e) {
				return e._receiver == receiver;
			}),
				_ctl->entries.end());
		}
		for (auto &s : states) {
			if (s && s->_disconnect_fn)
				s->_disconnect_fn();
		}
	}
	template <typename recv, typename D>
	void disconnect(const std::unique_ptr<recv, D> &receiver) {
		disconnect(static_cast<object *>(receiver.get()));
	}
	template <typename recv>
	void disconnect(const std::shared_ptr<recv> &receiver) {
		disconnect(static_cast<object *>(receiver.get()));
	}
	template <typename recv>
	void disconnect(const std::weak_ptr<recv> &receiver) {
		auto locked = receiver.lock();
		disconnect(static_cast<object *>(locked.get()));
	}

	void disconnect_all() {
		std::vector<std::shared_ptr<connection_state>> states;
		{
			std::lock_guard<std::mutex> lock(_ctl->mutex);
			for (auto &e : _ctl->entries) {
				if (e._state)
					states.push_back(e._state);
			}
			_ctl->entries.clear();
		}
		for (auto &s : states) {
			if (!s)
				continue;
			if (!s->_alive.exchange(false, std::memory_order_acq_rel))
				continue;
			if (s->_disconnect_fn)
				s->_disconnect_fn();
		}
	}

	/// 按值入参（调用处 decay-copy / move）；Queued 再按连接拷贝打包。
	/// move-only 参数：多个 Queued 连接时仅第一次 move
	/// 有效，宜单连接或改用可拷贝包装。
	void emit(Args... args) const {
		if (signals_blocked())
			return;
		thread_local std::vector<entry> tls_scratch;
		struct snapshot_guard {
			std::vector<entry> &tls;
			std::vector<entry> snapshot;
			explicit snapshot_guard(std::vector<entry> &t) : tls(t) {
				snapshot.swap(tls);
				snapshot.clear();
			}
			~snapshot_guard() {
				snapshot.clear();
				tls.swap(snapshot);
			}
		} guard(tls_scratch);
		auto &snapshot = guard.snapshot;
		{
			std::lock_guard<std::mutex> lock(_ctl->mutex);
			snapshot.reserve(_ctl->entries.size());
			for (const auto &e : _ctl->entries) {
				if (e._state && e._state->_alive.load(std::memory_order_acquire)) {
					snapshot.push_back(e);
				}
			}
		}

		auto make_bound_args = [&] {
			if constexpr ((std::is_copy_constructible_v<Args> && ...)) {
				return std::make_tuple(args...);
			} else {
				return std::make_tuple(std::move(args)...);
			}
		};

		auto invoke_queued = [](const std::shared_ptr<slot> &bound_slot,
			auto &&bound_args, const std::weak_ptr<void> &weak,
			object *receiver) {
			auto gate = weak.lock();
			if (!gate)
				return;
			if (receiver && !receiver->is_valid())
				return;
			std::apply(*bound_slot, std::move(bound_args));
		};

		for (const auto &e : snapshot) {
			if (!e._state || !e._slot ||
				!e._state->_alive.load(std::memory_order_acquire))
				continue;

			std::shared_ptr<void> gate;
			std::shared_ptr<event_loop> target_loop;
			utils::thread *target_thread = nullptr;
			if (e._receiver) {
				gate = e._receiver_alive.lock();
				if (!gate)
					continue;
				if (!e._receiver->is_valid())
					continue;
				target_thread = e._receiver->thread();
				target_loop = e._receiver->loop_shared();
			} else if (e._receiver_alive.expired()) {
				continue;
			}

			const bool same_thread =
				(target_thread != nullptr && target_thread == ensure_thread());

			bool use_direct = false;
			bool use_blocking = false;
			if (e._type == connection_type::queued) {
				if (!target_loop)
					continue;
				use_direct = false;
			} else if (e._type == connection_type::blocking_queued) {
				if (same_thread) {
					use_direct = true;
				} else if (!target_loop || !target_loop->is_pumping()) {
					continue;
				} else {
					use_blocking = true;
				}
			} else if (e._type == connection_type::direct) {
				if (same_thread) {
					use_direct = true;
				} else if (!target_loop) {
					continue;
				} else {
					use_direct = false;
				}
			} else {
				if (same_thread) {
					use_direct = true;
				} else if (!target_loop) {
					continue;
				} else {
					use_direct = false;
				}
			}

			if (use_direct) {
				if (e._receiver && (!gate || !e._receiver->is_valid()))
					continue;
				(*e._slot)(args...);
			} else if (use_blocking) {
				if (!target_loop)
					continue;
				(void)target_loop->post_blocking(
					[bound_slot = e._slot, bound_args = make_bound_args(),
						weak = e._receiver_alive, receiver = e._receiver,
						invoke_queued]() mutable {
					invoke_queued(bound_slot, std::move(bound_args), weak,
						receiver);
				});
			} else {
				if (!target_loop)
					continue;
				(void)target_loop->post(
					[bound_slot = e._slot, bound_args = make_bound_args(),
						weak = e._receiver_alive, receiver = e._receiver,
						invoke_queued]() mutable {
					invoke_queued(bound_slot, std::move(bound_args), weak,
						receiver);
				});
			}
		}
	}

	void operator()(Args... args) const { emit(std::move(args)...); }

private:
	template <typename recv, typename Method>
	connection connect_pmf(recv *receiver, Method method, connection_type type,
		bool unique) {
		static_assert(std::is_base_of_v<object, recv>,
			"receiver must derive from utils::object");
		if (!receiver || !method)
			return {};
		object *obj = static_cast<object *>(receiver);
		return add_connection(
			obj,
			[obj, receiver, method](const Args &...args) {
			if (!obj->is_valid())
				return;
			(void)(receiver->*method)(args...);
		},
			type, unique, make_pmf_key(obj, method));
	}

	struct slot_key {
		object *receiver = nullptr;
		const std::type_info *pmf_type = nullptr;
		std::shared_ptr<void> pmf;
		bool (*equal)(const void *, const void *) = nullptr;

		bool matches(const slot_key &other) const noexcept {
			if (receiver != other.receiver || !equal || !other.equal) {
				return false;
			}
			if (pmf_type != other.pmf_type || !pmf || !other.pmf)
				return false;
			return equal(pmf.get(), other.pmf.get());
		}
	};

	/// 整条连接的各种信息,包括连接状态,信号和槽函数对象,连接类型,槽函数键
	struct entry {
		std::shared_ptr<connection_state> _state;
		object *_receiver = nullptr;
		std::weak_ptr<void> _receiver_alive;
		std::shared_ptr<slot> _slot;
		connection_type _type = connection_type::automatic;
		slot_key _key;
	};

	struct control {
		mutable std::mutex mutex;
		std::vector<entry> entries;
		std::uint64_t next_id = 1;
	};

	template <class M> static slot_key make_pmf_key(object *receiver, M method) {
		slot_key key;
		key.receiver = receiver;
		key.pmf_type = &typeid(M);
		key.pmf = std::make_shared<M>(method);
		key.equal = [](const void *a, const void *b) {
			return *static_cast<const M *>(a) == *static_cast<const M *>(b);
		};
		return key;
	}

	connection add_connection(object *receiver, slot slot_, connection_type type,
		bool unique, slot_key key) {
		auto state = std::make_shared<connection_state>();
		std::uint64_t id = 0;
		{
			std::lock_guard<std::mutex> lock(_ctl->mutex);
			if (unique && key.equal) {
				for (const auto &e : _ctl->entries) {
					if (e._state && e._state->_alive.load(std::memory_order_acquire) &&
						e._key.matches(key)) {
						return {};
					}
				}
			}
			id = _ctl->next_id++;
			state->_id = id;
			std::weak_ptr<control> wctl = _ctl;
			std::weak_ptr<void> wrecv =
				receiver ? receiver->lifetime() : std::weak_ptr<void>{};
			state->_disconnect_fn = [wctl, id, wrecv, receiver]() {
				if (auto ctl = wctl.lock()) {
					std::lock_guard<std::mutex> lock(ctl->mutex);
					ctl->entries.erase(std::remove_if(ctl->entries.begin(),
						ctl->entries.end(),
						[id](const entry &e) {
						return e._state && e._state->_id == id;
					}),
						ctl->entries.end());
				}
				if (receiver && wrecv.lock())
					receiver->untrack_inbound(id);
			};

			entry e;
			e._state = state;
			e._receiver = receiver;
			e._receiver_alive =
				receiver ? receiver->lifetime() : std::weak_ptr<void>(_forever);
			e._slot = std::make_shared<slot>(std::move(slot_));
			e._type = type;
			e._key = std::move(key);
			_ctl->entries.push_back(std::move(e));
		}

		if (receiver && !receiver->track_inbound(state)) {
			if (state->_alive.exchange(false, std::memory_order_acq_rel) &&
				state->_disconnect_fn) {
				state->_disconnect_fn();
			}
			return {};
		}
		return connection{std::move(state)};
	}

	std::shared_ptr<control> _ctl = std::make_shared<control>();
	object *_owner = nullptr;
	std::weak_ptr<void> _owner_alive;
	std::atomic<bool> _blocked{false};
	std::shared_ptr<void> _forever = std::make_shared<char>('\0');
};

// =============================================================================
// invoke：槽成员函数取返回值；普通函数/lambda 取返回值；或投递 void 任务
// =============================================================================
	namespace detail {

	template <class M> struct slot_pmf_traits {};

	template <class C, class R, class... A>
	struct slot_pmf_traits<slots_t<R> (C::*)(A...)> {
		using value_type = R;
	};

	template <class C, class R, class... A>
	struct slot_pmf_traits<slots_t<R> (C::*)(A...) const> {
		using value_type = R;
	};

	/// 解析 Direct / BlockingQueued；无法同步取值时返回 error
	struct invoke_route {
		bool use_direct = false;
		bool use_blocking = false;
		std::errc error = std::errc{};
		bool failed() const noexcept { return error != std::errc{}; }
	};

	inline invoke_route resolve_invoke_route(object *obj, connection_type type) {
		invoke_route r;
		if (!obj) {
			r.error = std::errc::invalid_argument;
			return r;
		}
		auto loop_ = obj->loop_shared();
		const bool same_thread = (obj->thread() == ensure_thread());

		if (type == connection_type::queued) {
			r.error = std::errc::operation_in_progress;
			return r;
		}
		if (type == connection_type::blocking_queued) {
			if (same_thread) {
				r.use_direct = true;
			} else if (!loop_ || !loop_->is_pumping()) {
				r.error = std::errc::operation_not_permitted;
			} else {
				r.use_blocking = true;
			}
			return r;
		}
		if (type == connection_type::direct) {
			if (same_thread) {
				r.use_direct = true;
			} else if (!loop_) {
				r.error = std::errc::operation_not_permitted;
			} else {
				r.error = std::errc::operation_in_progress;
			}
			return r;
		}
		if (same_thread) {
			r.use_direct = true;
		} else if (!loop_) {
			r.error = std::errc::operation_not_permitted;
		} else {
			r.error = std::errc::operation_in_progress;
		}
		return r;
	}

	template <class Recv, class Method, class... Args>
	result<typename slot_pmf_traits<Method>::value_type>
	invoke_slot(Recv *receiver, Method method, connection_type type,
		Args &&...args) {
		using R = typename slot_pmf_traits<Method>::value_type;
		static_assert(std::is_base_of_v<object, Recv>,
			"receiver must derive from utils::object");
		if (!receiver || !method)
			return result_err(std::errc::invalid_argument);

		object *obj = static_cast<object *>(receiver);
		const invoke_route route = resolve_invoke_route(obj, type);
		if (route.failed())
			return result_err(route.error);

		auto call = [&]() -> result<R> {
			auto gate = obj->lifetime().lock();
			if (!gate || !obj->is_valid()) {
				return result_err(std::errc::owner_dead);
			}
			if constexpr (std::is_void_v<R>) {
				(receiver->*method)(std::forward<Args>(args)...);
				return result_ok();
			} else {
				return result_ok((receiver->*method)(std::forward<Args>(args)...).get());
			}
		};

		if (route.use_direct)
			return call();
		if (!route.use_blocking)
			return result_err(std::errc::operation_in_progress);

		auto loop_ = obj->loop_shared();
		if (!loop_)
			return result_err(std::errc::operation_not_permitted);
		auto out =
			std::make_shared<result<R>>(result_err(std::errc::operation_canceled));
		std::tuple<std::decay_t<Args>...> bound_args{std::forward<Args>(args)...};
		auto weak = obj->lifetime();
		const bool posted = loop_->post_blocking([receiver, method,
			bound_args = std::move(bound_args),
			weak, obj, out]() mutable {
			auto gate = weak.lock();
			if (!gate || !obj->is_valid()) {
				*out = result<R>(result_err(std::errc::owner_dead));
				return;
			}
			if constexpr (std::is_void_v<R>) {
				std::apply(
					[&](auto &&...a) {
					(receiver->*method)(std::forward<decltype(a)>(a)...);
				},
					std::move(bound_args));
				*out = result_ok();
			} else {
				*out = result_ok(std::apply(
					[&](auto &&...a) {
					return (receiver->*method)(std::forward<decltype(a)>(a)...).get();
				},
					std::move(bound_args)));
			}
		});
		if (!posted)
			return result_err(std::errc::operation_not_permitted);
		return std::move(*out);
	}

	template <class F, class... Args>
	using invoke_callable_result_t =
		std::decay_t<std::invoke_result_t<std::decay_t<F> &, Args...>>;

	template <class F, class... Args>
	result<invoke_callable_result_t<F, Args...>>
	invoke_callable(object *receiver, connection_type type, F &&fn,
		Args &&...args) {
		using R = invoke_callable_result_t<F, Args...>;
		if (!receiver)
			return result_err(std::errc::invalid_argument);

		const invoke_route route = resolve_invoke_route(receiver, type);
		if (route.failed())
			return result_err(route.error);

		auto fn_store = std::decay_t<F>(std::forward<F>(fn));

		auto call = [&]() -> result<R> {
			auto gate = receiver->lifetime().lock();
			if (!gate || !receiver->is_valid()) {
				return result_err(std::errc::owner_dead);
			}
			if constexpr (std::is_void_v<R>) {
				std::invoke(fn_store, std::forward<Args>(args)...);
				return result_ok();
			} else {
				return result_ok(std::invoke(fn_store, std::forward<Args>(args)...));
			}
		};

		if (route.use_direct)
			return call();
		if (!route.use_blocking)
			return result_err(std::errc::operation_in_progress);

		auto loop_ = receiver->loop_shared();
		if (!loop_)
			return result_err(std::errc::operation_not_permitted);
		auto out =
			std::make_shared<result<R>>(result_err(std::errc::operation_canceled));
		std::tuple<std::decay_t<Args>...> bound_args{std::forward<Args>(args)...};
		auto weak = receiver->lifetime();
		const bool posted = loop_->post_blocking([fn_store = std::move(fn_store),
			bound_args = std::move(bound_args),
			weak, receiver, out]() mutable {
			auto gate = weak.lock();
			if (!gate || !receiver->is_valid()) {
				*out = result<R>(result_err(std::errc::owner_dead));
				return;
			}
			if constexpr (std::is_void_v<R>) {
				std::apply(
					[&](auto &&...a) {
					std::invoke(fn_store, std::forward<decltype(a)>(a)...);
				},
					std::move(bound_args));
				*out = result_ok();
			} else {
				*out = result_ok(std::apply(
					[&](auto &&...a) {
					return std::invoke(fn_store, std::forward<decltype(a)>(a)...);
				},
					std::move(bound_args)));
			}
		});
		if (!posted)
			return result_err(std::errc::operation_not_permitted);
		return std::move(*out);
	}

} // namespace detail

/** @brief 调用成员槽并取回 slots_t 中的值（Queued / 跨线程 Auto 无法同步取值）
*/
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(Recv *receiver, slots_t<R> (C::*method)(SlotArgs...),
	connection_type type, SlotArgs... args) {
	return detail::invoke_slot(receiver, method, type,
		std::forward<SlotArgs>(args)...);
}

template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(Recv *receiver, slots_t<R> (C::*method)(SlotArgs...) const,
	connection_type type, SlotArgs... args) {
	return detail::invoke_slot(receiver, method, type,
		std::forward<SlotArgs>(args)...);
}

template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(Recv *receiver, slots_t<R> (C::*method)(SlotArgs...),
	SlotArgs... args) {
	return detail::invoke_slot(receiver, method, connection_type::automatic,
		std::forward<SlotArgs>(args)...);
}

template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(Recv *receiver, slots_t<R> (C::*method)(SlotArgs...) const,
	SlotArgs... args) {
	return detail::invoke_slot(receiver, method, connection_type::automatic,
		std::forward<SlotArgs>(args)...);
}

template <class Recv, class D, class C, class R, class... SlotArgs>
result<R> invoke(const std::unique_ptr<Recv, D> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), connection_type type,
	SlotArgs... args) {
	return invoke(receiver.get(), method, type, std::forward<SlotArgs>(args)...);
}
template <class Recv, class D, class C, class R, class... SlotArgs>
result<R> invoke(const std::unique_ptr<Recv, D> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), SlotArgs... args) {
	return invoke(receiver.get(), method, std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const std::shared_ptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), connection_type type,
	SlotArgs... args) {
	return invoke(receiver.get(), method, type, std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const std::shared_ptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), SlotArgs... args) {
	return invoke(receiver.get(), method, std::forward<SlotArgs>(args)...);
}

/**
* @brief 在 receiver 所在线程调用普通函数/lambda，并用 result 取回返回值。
* @note 必须带 connection_type，避免与 invoke(object*, function<void()>)
* 投递重载冲突。 Queued / 跨线程 Auto / 跨线程 Direct 无法同步取值 →
* operation_in_progress。
*/
template <class F, class... Args,
	std::enable_if_t<std::is_invocable_v<std::decay_t<F> &, Args...> &&
	!std::is_member_pointer_v<std::decay_t<F>>,
	int> = 0>
auto invoke(object *receiver, connection_type type, F &&fn, Args &&...args)
-> result<detail::invoke_callable_result_t<F, Args...>> {
	return detail::invoke_callable(receiver, type, std::forward<F>(fn),
		std::forward<Args>(args)...);
}

template <
	class Recv, class F, class... Args,
	std::enable_if_t<std::is_base_of_v<object, Recv> &&
	std::is_invocable_v<std::decay_t<F> &, Args...> &&
	!std::is_member_pointer_v<std::decay_t<F>>,
	int> = 0>
auto invoke(Recv *receiver, connection_type type, F &&fn, Args &&...args)
-> result<detail::invoke_callable_result_t<F, Args...>> {
	return detail::invoke_callable(static_cast<object *>(receiver), type,
		std::forward<F>(fn),
		std::forward<Args>(args)...);
}

template <class Recv, class D, class F, class... Args,
	std::enable_if_t<std::is_invocable_v<std::decay_t<F> &, Args...> &&
	!std::is_member_pointer_v<std::decay_t<F>>,
	int> = 0>
auto invoke(const std::unique_ptr<Recv, D> &receiver, connection_type type,
	F &&fn, Args &&...args)
-> result<detail::invoke_callable_result_t<F, Args...>> {
	return invoke(receiver.get(), type, std::forward<F>(fn),
		std::forward<Args>(args)...);
}

template <class Recv, class F, class... Args,
	std::enable_if_t<std::is_invocable_v<std::decay_t<F> &, Args...> &&
	!std::is_member_pointer_v<std::decay_t<F>>,
	int> = 0>
auto invoke(const std::shared_ptr<Recv> &receiver, connection_type type, F &&fn,
	Args &&...args)
-> result<detail::invoke_callable_result_t<F, Args...>> {
	return invoke(receiver.get(), type, std::forward<F>(fn),
		std::forward<Args>(args)...);
}

/// 把无返回值任务投递到 object 所在线程（不是槽，不走 slots_t，不返回 result）
inline void invoke(object *receiver, std::function<void()> fn,
	connection_type type = connection_type::automatic) {
	if (!receiver || !fn)
		return;

	auto loop_ = receiver->loop_shared();
	const bool same_thread = (receiver->thread() == ensure_thread());

	bool use_direct = false;
	bool use_blocking = false;
	if (type == connection_type::queued) {
		if (!loop_)
			return;
	} else if (type == connection_type::blocking_queued) {
		if (same_thread) {
			use_direct = true;
		} else if (!loop_ || !loop_->is_pumping()) {
			return;
		} else {
			use_blocking = true;
		}
	} else if (type == connection_type::direct) {
		if (same_thread) {
			use_direct = true;
		} else if (!loop_) {
			return;
		} else {
			use_direct = false;
		}
	} else {
		if (same_thread) {
			use_direct = true;
		} else if (!loop_) {
			return;
		} else {
			use_direct = false;
		}
	}

	if (use_direct) {
		auto gate = receiver->lifetime().lock();
		if (!gate || !receiver->is_valid())
			return;
		fn();
		return;
	}
	if (!loop_)
		return;

	auto weak = receiver->lifetime();
	auto wrapped = [weak = std::move(weak), fn = std::move(fn),
		receiver]() mutable {
		auto gate = weak.lock();
		if (!gate || !receiver->is_valid())
			return;
		fn();
	};
	if (use_blocking) {
		(void)loop_->post_blocking(std::move(wrapped));
	} else {
		(void)loop_->post(std::move(wrapped));
	}
}

template <typename T, typename D>
void invoke(const std::unique_ptr<T, D> &receiver, std::function<void()> fn,
	connection_type type = connection_type::automatic) {
	invoke(static_cast<object *>(receiver.get()), std::move(fn), type);
}
template <typename T>
void invoke(const std::shared_ptr<T> &receiver, std::function<void()> fn,
	connection_type type = connection_type::automatic) {
	invoke(static_cast<object *>(receiver.get()), std::move(fn), type);
}
template <typename T>
void invoke(const std::weak_ptr<T> &receiver, std::function<void()> fn,
	connection_type type = connection_type::automatic) {
	auto locked = receiver.lock();
	invoke(static_cast<object *>(locked.get()), std::move(fn), type);
}

// =============================================================================
// 自由函数 connect（仅成员槽，必须返回 slots_t<R>）
// =============================================================================
	template <typename... Args, typename recv, typename slot_class, typename R,
	typename... slot_args>
connection connect(signal<Args...> &signal_, recv *receiver,
	slots_t<R> (slot_class::*method)(slot_args...),
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, method, type, unique);
}

template <typename... Args, typename recv, typename slot_class, typename R,
	typename... slot_args>
connection connect(signal<Args...> &signal_, recv *receiver,
	slots_t<R> (slot_class::*method)(slot_args...) const,
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, method, type, unique);
}

template <typename... Args, typename recv, typename D, typename slot_class,
	typename R, typename... slot_args>
connection connect(signal<Args...> &signal_,
	const std::unique_ptr<recv, D> &receiver,
	slots_t<R> (slot_class::*method)(slot_args...),
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, method, type, unique);
}
template <typename... Args, typename recv, typename D, typename slot_class,
	typename R, typename... slot_args>
connection connect(signal<Args...> &signal_,
	const std::unique_ptr<recv, D> &receiver,
	slots_t<R> (slot_class::*method)(slot_args...) const,
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, method, type, unique);
}
template <typename... Args, typename recv, typename slot_class, typename R,
	typename... slot_args>
connection connect(signal<Args...> &signal_,
	const std::shared_ptr<recv> &receiver,
	slots_t<R> (slot_class::*method)(slot_args...),
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, method, type, unique);
}
template <typename... Args, typename recv, typename slot_class, typename R,
	typename... slot_args>
connection connect(signal<Args...> &signal_,
	const std::shared_ptr<recv> &receiver,
	slots_t<R> (slot_class::*method)(slot_args...) const,
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, method, type, unique);
}
template <typename... Args, typename recv, typename slot_class, typename R,
	typename... slot_args>
connection connect(signal<Args...> &signal_,
	const std::weak_ptr<recv> &receiver,
	slots_t<R> (slot_class::*method)(slot_args...),
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, method, type, unique);
}
template <typename... Args, typename recv, typename slot_class, typename R,
	typename... slot_args>
connection connect(signal<Args...> &signal_,
	const std::weak_ptr<recv> &receiver,
	slots_t<R> (slot_class::*method)(slot_args...) const,
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, method, type, unique);
}

template <typename... Args, typename recv, typename F,
	std::enable_if_t<
	std::is_base_of_v<object, recv> &&
	std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
	!std::is_member_function_pointer_v<std::decay_t<F>>,
	int> = 0>
connection connect(signal<Args...> &signal_, recv *receiver, F &&func,
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, std::forward<F>(func), type, unique);
}

template <
	typename... Args, typename recv, typename D, typename F,
	std::enable_if_t<std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
	!std::is_member_function_pointer_v<std::decay_t<F>>,
	int> = 0>
connection connect(signal<Args...> &signal_,
	const std::unique_ptr<recv, D> &receiver, F &&func,
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, std::forward<F>(func), type, unique);
}

template <
	typename... Args, typename recv, typename F,
	std::enable_if_t<std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
	!std::is_member_function_pointer_v<std::decay_t<F>>,
	int> = 0>
connection connect(signal<Args...> &signal_,
	const std::shared_ptr<recv> &receiver, F &&func,
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, std::forward<F>(func), type, unique);
}

template <
	typename... Args, typename recv, typename F,
	std::enable_if_t<std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
	!std::is_member_function_pointer_v<std::decay_t<F>>,
	int> = 0>
connection connect(signal<Args...> &signal_,
	const std::weak_ptr<recv> &receiver, F &&func,
	connection_type type = connection_type::automatic,
	bool unique = false) {
	return signal_.connect(receiver, std::forward<F>(func), type, unique);
}


} // namespace utils

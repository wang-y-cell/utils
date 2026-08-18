#pragma once

/**
 * signal_and_slots — 轻量 Qt 风格信号/槽 + 事件循环（C++17 header-only）
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
 *   - thread / ensure_thread：线程亲和句柄（仿 QThread / currentThread）
 *   - worker_thread：后台线程（继承 thread）；禁止在工作线程内 stop
 *   - core_application：主线程注册 ensure_thread()
 *   - ensure_thread_loop()：兼容薄包装，等价 ensure_thread()->loop()
 *   - connect 语法糖；emit / connect 丢弃槽返回值
 *   - invoke(槽)：Direct / BlockingQueued 用 result<T> 取回 slots_t 中的值
 *   - invoke(object*, type, fn, args...)：在目标线程调用普通函数/lambda，result
 *     取返回值
 *   - invoke(object*, function<void()>)：只投递无返回值任务（不取 result）
 *
 * 命名空间：utils（旧名 qto 仍可用别名兼容）
 *
 * 使用注意：
 *   1) 跨线程 object 销毁前先 worker_thread::stop() / 排空队列
 *   2) 派生类析构第一行必须 invalidate()（会断连；若当前在亲和线程则
 *      process_events 排空）（堆对象优先 object_uptr / delete_later，可减少手动
 *      invalidate）
 *   3) 跨线程 Direct 会自动降级为 Queued；Queued/BlockingQueued 无 loop 则丢弃
 *      BlockingQueued 要求目标 loop 正在 run；对正在泵的本 loop post_blocking
 *      会失败（防自死锁）
 *   4) 禁止在工作线程里调用 worker_thread::stop()（硬失败，不销毁 loop）
 *   5) 主线程建议先构造 core_application（或依赖 object 内 ensure_thread）
 *   6) 堆上 object 建议只用 object_uptr/object_sptr 拥有；connect 仅观察不延长寿命
 *   7) 亲和只绑 utils::thread*（move_to_thread）；object::thread() 返回 thread*，
 *      投递用 object::loop()。绑 worker 后 stop 再 Queued emit 安全跳过，再 start
 *      自动绑新 loop。
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
template <class T = void> class slots_t {
	T value_{};

public:
	slots_t() = default;
	slots_t(const slots_t &) = default;
	slots_t(slots_t &&) = default;
	slots_t &operator=(const slots_t &) = default;
	slots_t &operator=(slots_t &&) = default;
	operator T() const noexcept { return value_; }

	template <class U,
		std::enable_if_t<!std::is_same_v<std::decay_t<U>, slots_t> &&
		std::is_constructible_v<T, U>,
		int> = 0>
	slots_t(U &&v) : value_(std::forward<U>(v)) {}

	T &get() & noexcept { return value_; }
	const T &get() const & noexcept { return value_; }
	T get() && noexcept { return std::move(value_); }
};

template <> class slots_t<void> {
	public:
		slots_t() = default;
};

class event_loop;
class object;
class thread;

/**
* 每线程可有默认 event_loop（tls_default_loop）与正在泵的
* loop（tls_running_loop）。 线程亲和身份见 utils::thread /
* ensure_thread()；loop 仅作投递。
*/
inline thread_local event_loop *tls_default_loop = nullptr;
inline thread_local event_loop *tls_running_loop = nullptr;
/// 当前 OS 线程的 utils::thread 句柄（worker 运行时指向该 worker）
inline thread_local thread *tls_current_thread = nullptr;

/// 当前线程 loop：优先返回正在泵的，否则返回默认 loop
inline event_loop *current_thread_loop() noexcept {
	if (tls_running_loop)
		return tls_running_loop;
	return tls_default_loop;
}

// 前向声明；定义在 event_loop / thread 类之后
inline event_loop *ensure_thread_loop();
inline thread *ensure_thread();

namespace detail {

/// 泵 loop 时临时占用 TLS；退出时必须还原，避免栈上 loop 把默认指针留成悬空。
struct event_loop_tls_scope {
	event_loop *prev_running;
	event_loop *prev_default;

	explicit event_loop_tls_scope(event_loop *self) noexcept
		: prev_running(tls_running_loop), prev_default(tls_default_loop) {
		tls_running_loop = self;
		if (!tls_default_loop)
			tls_default_loop = self;
	}

	~event_loop_tls_scope() {
		tls_running_loop = prev_running;
		tls_default_loop = prev_default;
	}

	event_loop_tls_scope(const event_loop_tls_scope &) = delete;
	event_loop_tls_scope &operator=(const event_loop_tls_scope &) = delete;
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

	/** @brief 将任务添加到任务队列中 */
	void post(task task_) { (void)post_impl(std::move(task_)); }

	/**
	*@brief 将阻塞任务添加到队列中，添加成功则返回true，否则返回false
	* 此函数会阻塞，直到任务执行完成
	*@param task_ 阻塞任务
	*@return 添加成功则返回true，否则返回false
	*/
	bool post_blocking(task task_) {
		if (!task_)
			return false;
		// 正在泵本 loop 时再阻塞等待自己 → 死锁；拒绝投递
		if (tls_running_loop == this)
			return false;
		auto state = std::make_shared<blocking_post_state>();
		if (!post_impl([task_ = std::move(task_), state]() mutable {
			try {
				task_();
			} catch (...) {
				std::lock_guard<std::mutex> lock(state->mutex);
				state->error = std::current_exception();
				state->done = true;
				state->cv.notify_one();
				return;
			}
			std::lock_guard<std::mutex> lock(state->mutex);
			state->done = true;
			state->cv.notify_one();
		})) {
			return false;
		}
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			state->cv.wait(lock, [&] { return state->done; });
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

	/** @brief 运行事件循环 */
	void run() {
		detail::event_loop_tls_scope tls(this);
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
					if (!_tasks.empty()) { // 任务队列有任务
						task_ = std::move(_tasks.front());
						_tasks.pop();
						break;
					}
					// 任务队列没有任务了
					if (!_running) {
						return;
					}
					if (!_timers.empty()) { // 没有任务，但是有定时任务，等待
						_cv.wait_until(lock, _timers.top()._when);
					} else {
						_cv.wait(lock); // 任务队列没有任务，也没有定时任务
					}
				}
			}
			if (task_)
				task_(); // 执行任务
		}
	}

	/** @brief 停止事件循环，唤醒所有被当前loop阻塞的线程 */
	void stop() {
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_running = false;
		}
		_cv.notify_all();
	}

	/** @brief 判断当前loop是否正在运行 */
	bool is_running() const {
		std::lock_guard<std::mutex> lock(_mutex);
		return _running;
	}

	/**
	* @brief 处理已到期的定时器与已排队任务；可选最长等待。会临时设置 tls。
	* @note 无 budget：只排空当前到期/已排队任务，不等待未来定时器（供 invalidate
	* 排空）。 有 budget：空闲时可 wait 到下一定时器或 deadline。
	*/
	void process_events(std::optional<clock::duration> budget = std::nullopt) {
		const auto deadline =
			budget ? std::optional<clock::time_point>(clock::now() + *budget)
				: std::nullopt;
		detail::event_loop_tls_scope tls(this);

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
			if (task_)
				task_();
		}
	}

private:
	struct blocking_post_state {
		std::mutex mutex;
		std::condition_variable cv;
		bool done = false;
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
					body();
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
	timer_id _next_timer_id = 1;
};

// =============================================================================
// thread：线程亲和句柄（仿 QThread）；current_thread / worker_thread
// =============================================================================
class thread {
public:
	thread(const thread &) = delete;
	thread &operator=(const thread &) = delete;
	virtual ~thread() = default;

	virtual event_loop *loop() const = 0;
	virtual bool is_running() const = 0;

	std::weak_ptr<void> identity() const noexcept { return _identity; }

protected:
	thread() = default;
	std::shared_ptr<void> _identity{std::make_shared<char>('\0')};
};

/// 代表「当前 OS 线程」的句柄（不 spawn）；拥有本线程默认 event_loop
class current_thread final : public thread {
public:
	current_thread() : _loop(std::make_unique<event_loop>()) {
		tls_default_loop = _loop.get();
	}

	~current_thread() override {
		if (tls_default_loop == _loop.get())
			tls_default_loop = nullptr;
		if (tls_current_thread == this)
			tls_current_thread = nullptr;
	}

	event_loop *loop() const override { return _loop.get(); }
	bool is_running() const override { return _loop && _loop->is_running(); }

private:
	std::unique_ptr<event_loop> _loop;
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

/// 当前线程默认 event_loop（兼容旧调用；等价于 ensure_thread()->loop()）
inline event_loop *ensure_thread_loop() { return ensure_thread()->loop(); }

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
	/** @brief 执行事件循环 */
	int exec() {
		event_loop *loop_ = loop();
		if (loop_)
			loop_->run();
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

	void start() {
		std::lock_guard<std::mutex> lock(_mutex);
		if (_std_thread.joinable())
			return;
		_loop = std::make_unique<event_loop>();
		event_loop *loop_ = _loop.get();
		_std_thread = std::thread([this, loop_]() {
			tls_default_loop = loop_;
			tls_current_thread = this; // 本 OS 线程的 ensure_thread() → 本 worker
			loop_->run();
			tls_current_thread = nullptr;
			tls_default_loop = nullptr;
		});
	}

	void stop() {
		std::unique_ptr<event_loop> loop_;
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

	event_loop *loop() const override {
		std::lock_guard<std::mutex> lock(_mutex);
		return _loop.get();
	}

	bool is_running() const override {
		std::lock_guard<std::mutex> lock(_mutex);
		return _std_thread.joinable() && _loop && _loop->is_running();
	}

private:
	mutable std::mutex _mutex;
	std::unique_ptr<event_loop> _loop;
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
	void invalidate() noexcept {
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
		event_loop *loop_ = loop();
		if (loop_ && current_thread_loop() == loop_) {
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

	/// 解析出的投递 loop；绑 worker 且已 stop / 句柄已毁时可能为 nullptr
	event_loop *loop() const noexcept {
		utils::thread *t = thread();
		return t ? t->loop() : nullptr;
	}

	/// 若亲和为 worker_thread 则返回之，否则 nullptr
	worker_thread *worker() const noexcept {
		return dynamic_cast<worker_thread *>(thread());
	}

	void delete_later() {
		event_loop *loop_ = loop();
		if (!loop_) {
			delete this;
			return;
		}
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

	void block_signals(bool block) noexcept {
		_signals_blocked.store(block, std::memory_order_release);
	}
	[[nodiscard]] bool signals_blocked() const noexcept {
		return _signals_blocked.load(std::memory_order_acquire);
	}

	void track_inbound(const std::shared_ptr<connection_state> &state) {
		std::lock_guard<std::mutex> lock(_inbound_mutex);
		_inbound.push_back(state);
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
			std::lock_guard<std::mutex> lock(_mutex);
			for (auto &e : _entries) {
				if (e._receiver != receiver || !e._state)
					continue;
				if (e._state->_alive.exchange(false, std::memory_order_acq_rel)) {
					states.push_back(e._state);
				}
			}
			_entries.erase(std::remove_if(_entries.begin(), _entries.end(),
				[receiver](const entry &e) {
				return e._receiver == receiver;
			}),
				_entries.end());
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
			std::lock_guard<std::mutex> lock(_mutex);
			for (auto &e : _entries) {
				if (e._state)
					states.push_back(e._state); // 如果连接状态不为空，则添加到states中
			}
			_entries.clear(); // 清空_entries
		}
		for (auto &s : states) {
			if (!s)
				continue;
			if (!s->_alive.exchange(false, std::memory_order_acq_rel))
				continue;
			// _disconnect_fn 会再次抢 _mutex 移除（已空）并 untrack
			if (s->_disconnect_fn)
				s->_disconnect_fn(); // 如果连接状态的_disconnect_fn不为空，则调用它
		}
	}

	/// 按值入参（调用处 decay-copy / move）；Queued 再按连接拷贝打包。
	/// move-only 参数：多个 Queued 连接时仅第一次 move
	/// 有效，宜单连接或改用可拷贝包装。
	void emit(Args... args) const {
		if (signals_blocked()) // 如果当前信号是阻塞状态,也就是说这个信号是blocking类型且被阻塞了,则直接返回,不进行emit
			return;
		// thread_local的生命周期是伴随整个线程的,这是一个长期存活的对象
		thread_local std::vector<entry> tls_scratch;
		std::vector<entry> snapshot;
		snapshot.swap(tls_scratch); // snapshot获得tls_scratch的值,并清空tls_scratch
		snapshot.clear();
		{
			std::lock_guard<std::mutex> lock(_mutex);
			snapshot.reserve(
				_entries.size()); // 将这个临时vector的容量设置为_entries的大小
			for (const auto &e : _entries) {
				// 如果连接状态不为空且连接状态存活,则将这个连接添加到snapshot中
				if (e._state && e._state->_alive.load(std::memory_order_acquire)) {
					snapshot.push_back(e);
				}
			}
		}

		// 将参数打包成一个tuple
		auto make_bound_args = [&] {
			if constexpr ((std::is_copy_constructible_v<Args> && ...)) {
				return std::make_tuple(args...);
			} else {
				return std::make_tuple(std::move(args)...);
			}
		};

		// 遍历snapshot中的每个连接,这是遍历每个槽函数连接
		for (const auto &e : snapshot) {
			// 如果连接状态不存活,则直接跳过
			if (!e._state->_alive.load(std::memory_order_acquire))
				continue;

			event_loop *target_loop = nullptr;
			if (e._receiver) { // 如果接收者不为空,则获取接收者的生存期
				auto gate = e._receiver_alive.lock(); // 获取接收者的生存期
				if (!gate) // 如果接收者的生存期为空,则直接跳过
					continue;
				if (!e._receiver->is_valid()) // 如果接收者不有效,则直接跳过
					continue;
				target_loop = e._receiver->loop(); // 获取接收者的亲和 loop
			} else if (e._receiver_alive
			.expired()) { // 如果接收者的生存期为空,则直接跳过
				continue;
			}

			// 判断当前线程是否与接收者在同一个线程
			const bool same_thread = (target_loop == current_thread_loop());

			bool use_direct = false;   // 是否使用直接调用
			bool use_blocking = false; // 是否使用阻塞调用
			if (e._type == connection_type::queued) {
				if (!target_loop) // 如果亲和loop为空,则直接跳过
					continue;
				use_direct = false;
			} else if (e._type == connection_type::blocking_queued) {
				if (same_thread) {
					use_direct = true;
				} else if (!target_loop || !target_loop->is_running()) {
					continue;
				} else {
					use_blocking = true;
				}
			} else if (e._type == connection_type::direct) {
				if (same_thread || !target_loop) {
					use_direct = true;
				} else {
					use_direct = false;
				}
			} else {
				use_direct = same_thread || !target_loop;
			}

			if (use_direct) {
				if (e._receiver &&
					(!e._receiver->is_valid() || e._receiver_alive.expired())) {
					continue;
				}
				e._slot(args...);
			} else if (use_blocking) {
				if (!target_loop)
					continue;
				auto bound_slot = e._slot;
				auto bound_args = make_bound_args();
				auto weak = e._receiver_alive;
				object *receiver = e._receiver;
				(void)target_loop->post_blocking([bound_slot = std::move(bound_slot),
					bound_args = std::move(bound_args),
					weak, receiver]() mutable {
					if (!weak.lock())
						return;
					if (receiver && !receiver->is_valid())
						return;
					std::apply(bound_slot, std::move(bound_args));
				});
			} else {
				if (!target_loop)
					continue;
				auto bound_slot = e._slot;
				auto bound_args = make_bound_args();
				auto weak = e._receiver_alive;
				object *receiver = e._receiver;
				target_loop->post([bound_slot = std::move(bound_slot),
					bound_args = std::move(bound_args), weak,
					receiver]() mutable {
					if (!weak.lock())
						return;
					if (receiver && !receiver->is_valid())
						return;
					std::apply(bound_slot, std::move(bound_args));
				});
			}
		}

		snapshot.clear();
		tls_scratch.swap(snapshot);
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
		std::shared_ptr<connection_state> _state; // 连接状态
		object *_receiver = nullptr;              // 接收信号的object对象
		std::weak_ptr<void> _receiver_alive;      // 接收信号的object对象是否存活
		slot _slot;
		connection_type _type = connection_type::automatic;
		slot_key _key;
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
			std::lock_guard<std::mutex> lock(_mutex);
			if (unique && key.equal) {
				for (const auto &e : _entries) {
					if (e._state && e._state->_alive.load(std::memory_order_acquire) &&
						e._key.matches(key)) {
						return {};
					}
				}
			}
			id = _next_id++;
			state->_id = id;
			state->_disconnect_fn = [this, id, receiver]() {
				{
					std::lock_guard<std::mutex> lock(_mutex);
					_entries.erase(std::remove_if(_entries.begin(), _entries.end(),
						[id](const entry &e) {
						return e._state &&
							e._state->_id == id;
					}),
						_entries.end());
				}
				if (receiver)
					receiver->untrack_inbound(id);
			};

			entry e;
			e._state = state;
			e._receiver = receiver;
			// 无 receiver：用永不过期哨兵，避免 weak_ptr{} 的 expired()==true
			e._receiver_alive =
				receiver ? receiver->lifetime() : std::weak_ptr<void>(_forever);
			e._slot = std::move(slot_);
			e._type = type;
			e._key = std::move(key);
			_entries.push_back(std::move(e));
		}

		if (receiver)
			receiver->track_inbound(
				state); // 如果receiver不为空，则将连接状态添加到_inbound中
		return connection{std::move(state)};
	}

	mutable std::mutex _mutex;
	std::vector<entry> _entries; // 这个信号有多少槽函数连接
	std::uint64_t _next_id = 1;
	object *_owner = nullptr;          // 信号的拥有者
	std::weak_ptr<void> _owner_alive;  // 信号的拥有者是否存活
	std::atomic<bool> _blocked{false}; // 信号是否被阻塞
	// 无 receiver 连接的存活哨兵
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
		event_loop *loop_ = obj->loop();
		const bool same_thread = (loop_ == current_thread_loop());

		if (type == connection_type::queued) {
			r.error = std::errc::operation_in_progress;
			return r;
		}
		if (type == connection_type::blocking_queued) {
			if (same_thread) {
				r.use_direct = true;
			} else if (!loop_ || !loop_->is_running()) {
				r.error = std::errc::operation_not_permitted;
			} else {
				r.use_blocking = true;
			}
			return r;
		}
		if (type == connection_type::direct) {
			if (same_thread || !loop_) {
				r.use_direct = true;
			} else {
				r.error = std::errc::operation_in_progress;
			}
			return r;
		}
		// automatic
		if (same_thread || !loop_) {
			r.use_direct = true;
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
			if (!obj->is_valid() || !obj->lifetime().lock()) {
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

		event_loop *loop_ = obj->loop();
		auto out =
			std::make_shared<result<R>>(result_err(std::errc::operation_canceled));
		std::tuple<std::decay_t<Args>...> bound_args{std::forward<Args>(args)...};
		auto weak = obj->lifetime();
		const bool posted = loop_->post_blocking([receiver, method,
			bound_args = std::move(bound_args),
			weak, obj, out]() mutable {
			if (!weak.lock() || !obj->is_valid()) {
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
			if (!receiver->is_valid() || !receiver->lifetime().lock()) {
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

		event_loop *loop_ = receiver->loop();
		auto out =
			std::make_shared<result<R>>(result_err(std::errc::operation_canceled));
		std::tuple<std::decay_t<Args>...> bound_args{std::forward<Args>(args)...};
		auto weak = receiver->lifetime();
		const bool posted = loop_->post_blocking([fn_store = std::move(fn_store),
			bound_args = std::move(bound_args),
			weak, receiver, out]() mutable {
			if (!weak.lock() || !receiver->is_valid()) {
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

	event_loop *loop_ = receiver->loop();
	const bool same_thread = (loop_ == current_thread_loop());

	bool use_direct = false;
	bool use_blocking = false;
	if (type == connection_type::queued) {
		if (!loop_)
			return;
	} else if (type == connection_type::blocking_queued) {
		if (same_thread) {
			use_direct = true;
		} else if (!loop_ || !loop_->is_running()) {
			return;
		} else {
			use_blocking = true;
		}
	} else if (type == connection_type::direct) {
		use_direct = same_thread || !loop_;
	} else {
		use_direct = same_thread || !loop_;
	}

	if (use_direct) {
		if (!receiver->is_valid() || !receiver->lifetime().lock())
			return;
		fn();
		return;
	}
	if (!loop_)
		return;

	auto weak = receiver->lifetime();
	auto wrapped = [weak = std::move(weak), fn = std::move(fn),
		receiver]() mutable {
		if (!weak.lock() || !receiver->is_valid())
			return;
		fn();
	};
	if (use_blocking) {
		(void)loop_->post_blocking(std::move(wrapped));
	} else {
		loop_->post(std::move(wrapped));
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

} // namespace utils

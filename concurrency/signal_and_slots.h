#pragma once

/**
 * signal_and_slots — 轻量信号/槽 + 事件循环（C++20 header-only）
 *
 * 日常用法：
 *   #include "signal_and_slots.h"
 *   using namespace utils;
 *
 * 别名（utils）：
 *   sptr<T> = shared_ptr<T>；uptr<T> = unique_ptr<T>；wptr<T> = weak_ptr<T>
 *
 * 模型（相对 Qt / 旧版 object 亲和）：
 *   - 不要求接收者继承 object；connect / invoke 对任意类型可用
 *   - 接收者寿命仅通过 sptr / wptr（连接内部存 weak_ptr）；无 trackable、
 *     无裸指针 forever-token
 *   - 必须用自由函数 utils::connect(...)；signal::connect 为 private
 *   - 亲和表 unordered_map：receiver → (method_key → thread*)；
 *     method_key = type_index + PMF 字节；connect(..., thread*) 写入；
 *     invoke / 无显式 thread* 的 connect 查表；sptr 绑定存 wptr 并在 find 时校验
 *   - 每条连接在 connect 时固定目标 thread*（entry._target）；emit 的
 *     Queued / Auto / BlockingQueued 投递到该 thread 的 loop
 *   - 仅有 worker 式 thread（spawn OS 线程 + event_loop）；无
 *     current_thread 类、无 core_application、无 ensure_thread 伪造主线程
 *   - current_thread()：TLS 指针；主线程 / 非 worker 上为 nullptr
 *
 * 能力：
 *   - connection_type: Direct / Queued / BlockingQueued / Automatic
 *   - 成员槽须返回 slots_t / slots_t<T>；lambda connect 不要求 slots_t
 *   - unique_connection：同一接收者 + 同一成员槽只连一次（lambda 不做 unique）
 *   - signal 公开：ctor / dtor / emit / operator() / block_signals /
 *     signals_blocked / disconnect(connection&|sptr|wptr) / disconnect_all
 *   - connection / scoped_connection
 *   - event_loop：post / post_blocking / 定时器 / process_events；泵送吞异常
 *   - invoke：成员槽仅 sptr/wptr；另支持 thread* + callable；Queued / 跨线程
 *     Auto 无法同步取 result
 *
 * 命名空间：utils
 *
 * 使用注意：
 *   1) Automatic 且无绑定（target==nullptr）：按 Direct 在 emit/invoke 线程执行
 *   2) Queued / BlockingQueued 无 target（表中无绑定且未显式传入）：失败/跳过，
 *      不静默在当前线程执行
 *   3) connect / 成员 invoke 接收者必须为 sptr 或 wptr（不支持 uptr / 裸指针）
 *   4) 槽调用前 lock weak_ptr；寿命结束则跳过
 *   5) 禁止在本 worker 线程内 thread::stop()（硬失败）
 *   6) BlockingQueued 要求目标 loop 正在泵；仅当「当前线程正在泵本 loop」时
 *      post_blocking 因自死锁失败（tls_pumping_loop）
 *   7) Direct 始终在 emit 线程同步调用（跨线程亦然，调用方自担数据竞争）
 */
#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "reliability/expected.h"

namespace utils {

template <class T> using sptr = std::shared_ptr<T>;
template <class T> using uptr = std::unique_ptr<T>;
template <class T> using wptr = std::weak_ptr<T>;

enum class connection_type {
	direct,          // 同步：始终在 emit 线程执行（跨线程亦然，调用方自担安全）
	queued,          // 异步：投递到连接固定的 target thread 的 event_loop
	blocking_queued, // 跨线程投递并等待目标线程执行完成（要求目标 loop 正在 run）
	automatic        // 同线程或无绑定 → Direct；跨线程有绑定 → Queued
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

template <> class slots_t<void> {
public:
	slots_t() = default;
};

class event_loop;
class thread;
class connection;
struct connection_state;

/// 当前 OS 线程的 utils::thread 句柄（仅 worker 运行时非空）
inline thread_local thread *tls_current_thread = nullptr;
/// 当前 OS 线程正在泵的 event_loop（仅本线程可见；用于 post_blocking 防自死锁）
inline thread_local event_loop *tls_pumping_loop = nullptr;

/// 返回 TLS 中的 worker 指针；主线程 / 非 worker 为 nullptr（不伪造主线程）
inline thread *current_thread() noexcept { return tls_current_thread; }

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

/// 亲和表：(receiver → (method_key → value))，unordered_map
/// method_key = type_index(M) + PMF 字节；find 时校验 weak 寿命。
struct method_key {
	std::type_index type{typeid(void)}; // 成员函数指针的类型
	std::array<unsigned char, 32> bytes{}; // 成员函数转换成一段原始字节
	std::uint8_t size = 0;

	bool operator==(const method_key &o) const {
		return type == o.type && size == o.size && 
			std::memcmp(bytes.data(), o.bytes.data(), size) == 0;
	}
};

///哈希函数,通过一个method_key获得一个哈希值
struct method_key_hash {
	std::size_t operator()(const method_key &k) const noexcept {
		std::size_t h = k.type.hash_code(); //只标识成员函数指针的类型,不表示哪个成员函数
		//0x9e3779b9u（黄金比例的倒数）
		for(std::uint8_t i = 0; i < k.size; i++) {
			h ^= static_cast<std::size_t>(k.bytes[i]) + 0x9e3779b9u + (h << 6) + (h >> 2);
		}
		return h;
	}
};

struct affinity_value {
	bool has_receiver_alive = false; // 是否存在接收者
	std::weak_ptr<void> receiver_alive; // 接收者的生存状态
	thread *target = nullptr; // 目标线程
	std::weak_ptr<void> target_alive; // 目标线程的生存状态
};

using method_affinity_map =
	std::unordered_map<method_key, affinity_value, method_key_hash>;

///亲和表,双层hash表,第一层指向接收者对象,第二层指向这个对象的成员函数
using affinity_map = std::unordered_map<const void *, method_affinity_map>;

inline std::mutex &affinity_mutex() {
	static std::mutex m;
	return m;
}

inline affinity_map &affinity_bindings() {
	static affinity_map m;
	return m;
}

/// @brief 将成员函数转换成method_key,用于亲和表的查找
template <class M>
method_key make_method_key(M method) {
	static_assert(std::is_member_function_pointer_v<M>); //必须是成员函数
	static_assert(sizeof(M) <= 32); //成员函数大小不能超过32字节
	method_key k;
	k.type = std::type_index(typeid(M));
	//将这个函数的地址强制转换成一段原始字节,大小刚好是M字节的大小
	std::memcpy(k.bytes.data(), &method, sizeof(M));
	k.size = static_cast<std::uint8_t>(sizeof(M)); // 设置成员内存字节大小
	return k;
}

/// @brief 更新亲和表,根据接收者和成员函数更新表
/// @param receiver 接收者对象
/// @param method 成员函数
/// @param target 目标线程
/// @param target_alive 目标线程的生存状态,weak_ptr指向目标线程的对象
/// @param has_receiver_alive 是否存在接收者
/// @param receiver_alive 接收者的生存状态,weak_ptr指向接收者对象
template <class M>
void affinity_upsert(const void *receiver, M method, thread *t,
	std::weak_ptr<void> target_alive, bool has_receiver_alive,
	std::weak_ptr<void> receiver_alive) {
	auto &outer = affinity_bindings();
	auto &inner = outer[receiver];
	affinity_value &v = inner[make_method_key(method)];
	v.target = t;
	v.target_alive = std::move(target_alive);
	v.has_receiver_alive = has_receiver_alive;
	if (has_receiver_alive)
		v.receiver_alive = std::move(receiver_alive);
	else
		v.receiver_alive.reset();
}

} // namespace detail

// 亲和表公开 API（定义在 thread 类之后）
template <class M>
void bind_slot_affinity(const void *receiver, M method, thread *t);
template <class Recv, class M>
void bind_slot_affinity(const sptr<Recv> &receiver, M method, thread *t);
template <class Recv, class M>
void bind_slot_affinity(const wptr<Recv> &receiver, M method, thread *t);
template <class M>
thread *find_slot_affinity(const void *receiver, M method);
template <class Recv, class M>
thread *find_slot_affinity(const sptr<Recv> &receiver, M method);
template <class Recv, class M>
thread *find_slot_affinity(const wptr<Recv> &receiver, M method);
inline void clear_slot_affinity(const void *receiver);
template <class Recv>
void clear_slot_affinity(const sptr<Recv> &receiver);
template <class Recv>
void clear_slot_affinity(const wptr<Recv> &receiver);

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
	 * @brief 阻塞投递：成功入队则等待执行完成
	 * @note 当前线程正在泵本 loop 时拒绝（防自死锁）；目标未泵时失败
	 */
	bool post_blocking(task task_) {
		if (!task_)
			return false;
		if (tls_pumping_loop == this)
			return false;
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
					if (!_tasks.empty()) {
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
					task_();
				} catch (...) {
				}
			}
		}
	}

	/** @brief 停止事件循环，唤醒所有被当前 loop 阻塞的线程 */
	void stop() {
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_running = false;
			++_stop_epoch;
		}
		_cv.notify_all();
	}

	bool is_running() const {
		std::lock_guard<std::mutex> lock(_mutex);
		return _running;
	}

	void set_accepting(bool on) {
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_running = on;
		}
		if (on)
			_cv.notify_all();
	}

	bool is_pumping() const noexcept {
		return _pumping_depth.load(std::memory_order_acquire) > 0;
	}

	bool is_pumping_on_current_thread() const noexcept {
		return tls_pumping_loop == this;
	}

	/**
	 * @brief 处理已到期的定时器与已排队任务；可选最长等待。
	 * @note 无 budget：只排空当前到期/已排队任务。有 budget：空闲时可 wait。
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
					break;
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
		clock::duration _interval;

		bool operator>(const timer_item &o) const { return _when > o._when; }
	};

	void flush_due_timers_unlocked() {
		const auto now = clock::now();
		while (!_timers.empty() && _timers.top()._when <= now) {
			timer_item item = _timers.top();
			_timers.pop();
			if (_cancelled.erase(item._id) > 0)
				continue;

			if (item._interval > clock::duration::zero()) {
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
					_timers.push(timer_item{clock::now() + interval, id,
						std::move(body), interval});
					_cv.notify_one();
				});
			} else {
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
// thread：具体 worker（spawn OS 线程 + event_loop）
// =============================================================================
class thread {
public:
	thread() = default;
	thread(const thread &) = delete;
	thread &operator=(const thread &) = delete;

	~thread() { stop(); }

	void start() {
		std::lock_guard<std::mutex> lock(_mutex);
		if (_std_thread.joinable())
			return;
		_loop = std::make_shared<event_loop>();
		auto loop_ = _loop;
		_std_thread = std::thread([this, loop_]() {
			tls_current_thread = this;
			loop_->run();
			tls_current_thread = nullptr;
		});
	}

	/// 停止并 join；禁止在本 worker 线程内调用
	void stop() {
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
				"thread::stop() must not be called from its own thread");
			return;
		}
		if (loop_)
			loop_->stop();
		if (th.joinable())
			th.join();
	}

	std::shared_ptr<event_loop> loop_shared() const {
		std::lock_guard<std::mutex> lock(_mutex);
		return _loop;
	}

	event_loop *loop() const { return loop_shared().get(); }

	bool is_running() const {
		std::lock_guard<std::mutex> lock(_mutex);
		return _std_thread.joinable() && _loop && _loop->is_running();
	}

	std::weak_ptr<void> identity() const noexcept { return _identity; }

private:
	mutable std::mutex _mutex;
	std::shared_ptr<event_loop> _loop;
	std::thread _std_thread;
	std::shared_ptr<void> _identity{std::make_shared<char>('\0')};
};

using worker_thread = thread;

/// @brief 绑定槽函数与目标线程（unordered_map；已存在则覆盖 target）
/// @param receiver 接收者对象
/// @param method 成员函数
/// @param target 目标线程
/// @return 连接对象
template <class M>
void bind_slot_affinity(const void *receiver, M method, thread *t) {
	if (!receiver || !method)
		return;
	std::weak_ptr<void> target_alive =
		t ? t->identity() : std::weak_ptr<void>{};
	std::lock_guard<std::mutex> lock(detail::affinity_mutex());
	detail::affinity_upsert(receiver, method, t, std::move(target_alive),
		false, {});
}

/// @brief 绑定槽函数与目标线程
template <class Recv, class M>
void bind_slot_affinity(const sptr<Recv> &receiver, M method, thread *t) {
	if (!receiver || !method)
		return;
	const void *raw = receiver.get();
	std::weak_ptr<void> life(
		std::static_pointer_cast<void>(sptr<Recv>(receiver)));
	std::weak_ptr<void> target_alive =
		t ? t->identity() : std::weak_ptr<void>{};
	std::lock_guard<std::mutex> lock(detail::affinity_mutex());
	detail::affinity_upsert(raw, method, t, std::move(target_alive), true,
		std::move(life));
}

template <class Recv, class M>
void bind_slot_affinity(const wptr<Recv> &receiver, M method, thread *t) {
	auto locked = receiver.lock();
	if (!locked)
		return;
	bind_slot_affinity(locked, method, t);
}

/// @brief 查找与给定 receiver 和 method 匹配的 slot 的目标线程
template <class M>
thread *find_slot_affinity(const void *receiver, M method) {
	if (!receiver || !method)
		return nullptr;
	std::lock_guard<std::mutex> lock(detail::affinity_mutex());
	auto &outer = detail::affinity_bindings();
	auto oit = outer.find(receiver);
	if (oit == outer.end())
		return nullptr;
	auto mit = oit->second.find(detail::make_method_key(method));
	if (mit == oit->second.end())
		return nullptr;
	const detail::affinity_value &v = mit->second;
	if (v.has_receiver_alive && !v.receiver_alive.lock())
		return nullptr;
	if (!v.target)
		return nullptr;
	if (!v.target_alive.lock())
		return nullptr;
	return v.target;
}

template <class Recv, class M>
thread *find_slot_affinity(const sptr<Recv> &receiver, M method) {
	if (!receiver)
		return nullptr;
	return find_slot_affinity(static_cast<const void *>(receiver.get()), method);
}

template <class Recv, class M>
thread *find_slot_affinity(const wptr<Recv> &receiver, M method) {
	auto locked = receiver.lock();
	if (!locked)
		return nullptr;
	return find_slot_affinity(locked, method);
}

/// @brief 清除该 receiver 的全部绑定；并清掉 receiver_alive 已失效的条目
inline void clear_slot_affinity(const void *receiver) {
	std::lock_guard<std::mutex> lock(detail::affinity_mutex());
	auto &outer = detail::affinity_bindings();
	if (receiver)
		outer.erase(receiver);

	for (auto oit = outer.begin(); oit != outer.end();) {
		auto &inner = oit->second;
		for (auto mit = inner.begin(); mit != inner.end();) {
			if (mit->second.has_receiver_alive &&
				!mit->second.receiver_alive.lock()) {
				mit = inner.erase(mit);
			} else {
				++mit;
			}
		}
		if (inner.empty())
			oit = outer.erase(oit);
		else
			++oit;
	}
}

template <class Recv>
void clear_slot_affinity(const sptr<Recv> &receiver) {
	clear_slot_affinity(static_cast<const void *>(receiver.get()));
}

template <class Recv>
void clear_slot_affinity(const wptr<Recv> &receiver) {
	auto locked = receiver.lock();
	clear_slot_affinity(static_cast<const void *>(locked.get()));
}

/// @brief 连接状态（signal 与 connection 共享）
/// @brief 连接状态是连接的共享状态,用于存储连接的id和断开连接的函数
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

namespace detail {
struct signal_connecter {
	template <class Sig, class... A>
	static connection connect(Sig &s, A &&...a) {
		return s.connect(std::forward<A>(a)...);
	}
};
} // namespace detail

// =============================================================================
// signal
// =============================================================================
template <typename... Args> class signal {
	friend struct detail::signal_connecter;

public:
	/// @brief 槽函数类型,参数为信号的参数类型
	using slot = std::function<void(Args...)>;

	signal() noexcept = default;
	explicit signal(bool initially_blocked) noexcept : _blocked(initially_blocked) {}
	signal(const signal &) = delete;
	signal &operator=(const signal &) = delete;

	~signal() { disconnect_all(); }

	void block_signals(bool block) noexcept {
		_blocked.store(block, std::memory_order_release);
	}
	[[nodiscard]] bool signals_blocked() const noexcept {
		return _blocked.load(std::memory_order_acquire);
	}

	void disconnect(connection &c) { c.disconnect(); }

	template <typename Recv>
	void disconnect(const sptr<Recv> &receiver) {
		disconnect_receiver(static_cast<void *>(receiver.get()));
	}
	template <typename Recv>
	void disconnect(const wptr<Recv> &receiver) {
		auto locked = receiver.lock();
		disconnect_receiver(static_cast<void *>(locked.get()));
	}

	void disconnect_all() {
		std::vector<std::shared_ptr<connection_state>> states;
		{
			std::lock_guard<std::mutex> lock(_ctl->mutex);
			auto cur = _ctl->load_list();
			states.reserve(cur->size());
			for (const auto &e : *cur) {
				if (e._state)
					states.push_back(e._state);
			}
			_ctl->store_list(std::make_shared<entry_list>());
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

	void emit(Args... args) const {
		if (signals_blocked())
			return;

		_ctl->readers.fetch_add(1, std::memory_order_acquire);
		struct reader_guard {
			control *c;
			~reader_guard() {
				c->readers.fetch_sub(1, std::memory_order_release);
			}
		} guard{_ctl.get()};

		const entry_list *snap = _ctl->published.load(std::memory_order_acquire);
		if (!snap || snap->empty())
			return;

		auto make_bound_args = [&] {
			if constexpr ((std::is_copy_constructible_v<Args> && ...)) {
				return std::make_tuple(args...);
			} else {
				return std::make_tuple(std::move(args)...);
			}
		};

		auto invoke_queued = [](const std::shared_ptr<slot> &bound_slot,
								 auto &&bound_args,
								 const std::weak_ptr<void> &weak) {
			auto gate = weak.lock();
			if (!gate)
				return;
			std::apply(*bound_slot, std::move(bound_args));
		};

		thread *const self_thread = current_thread();

		for (const auto &e : *snap) {
			if (!e._state || !e._slot ||
				!e._state->_alive.load(std::memory_order_acquire))
				continue;

			{
				auto gate = e._receiver_alive.lock();
				if (!gate)
					continue;
			}

			if (e._type == connection_type::direct) {
				(*e._slot)(args...);
				continue;
			}

			thread *target = nullptr;
			if (e._target && e._target_alive.lock())
				target = e._target;

			const bool same_thread =
				(target != nullptr && target == self_thread);

			if (e._type == connection_type::automatic) {
				if (!target || same_thread) {
					(*e._slot)(args...);
					continue;
				}
				auto target_loop = target->loop_shared();
				if (!target_loop)
					continue;
				(void)target_loop->post(
					[bound_slot = e._slot, bound_args = make_bound_args(),
						weak = e._receiver_alive,
						invoke_queued]() mutable {
						invoke_queued(bound_slot, std::move(bound_args), weak);
					});
				continue;
			}

			if (e._type == connection_type::queued) {
				if (!target)
					continue;
				auto target_loop = target->loop_shared();
				if (!target_loop)
					continue;
				(void)target_loop->post(
					[bound_slot = e._slot, bound_args = make_bound_args(),
						weak = e._receiver_alive,
						invoke_queued]() mutable {
						invoke_queued(bound_slot, std::move(bound_args), weak);
					});
				continue;
			}

			if (e._type == connection_type::blocking_queued) {
				if (!target)
					continue;
				if (same_thread) {
					(*e._slot)(args...);
					continue;
				}
				auto target_loop = target->loop_shared();
				if (!target_loop || !target_loop->is_pumping())
					continue;
				(void)target_loop->post_blocking(
					[bound_slot = e._slot, bound_args = make_bound_args(),
						weak = e._receiver_alive,
						invoke_queued]() mutable {
						invoke_queued(bound_slot, std::move(bound_args), weak);
					});
			}
		}
	}

	void operator()(Args... args) const { emit(std::move(args)...); }

private:
	// ----- 成员槽：显式 thread* -----
	template <typename Recv, typename SlotClass, typename R, typename... SlotArgs>
	connection connect(const sptr<Recv> &receiver,
		slots_t<R> (SlotClass::*method)(SlotArgs...), thread *target,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect_pmf_shared(receiver, method, target, type, unique, true);
	}
	template <typename Recv, typename SlotClass, typename R, typename... SlotArgs>
	connection connect(const sptr<Recv> &receiver,
		slots_t<R> (SlotClass::*method)(SlotArgs...) const, thread *target,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect_pmf_shared(receiver, method, target, type, unique, true);
	}

	// ----- 成员槽：查亲和表 -----
	template <typename Recv, typename SlotClass, typename R, typename... SlotArgs>
	connection connect(const sptr<Recv> &receiver,
		slots_t<R> (SlotClass::*method)(SlotArgs...),
		connection_type type = connection_type::automatic,
		bool unique = false) {
		thread *target = find_slot_affinity(receiver, method);
		return connect_pmf_shared(receiver, method, target, type, unique, false);
	}
	template <typename Recv, typename SlotClass, typename R, typename... SlotArgs>
	connection connect(const sptr<Recv> &receiver,
		slots_t<R> (SlotClass::*method)(SlotArgs...) const,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		thread *target = find_slot_affinity(receiver, method);
		return connect_pmf_shared(receiver, method, target, type, unique, false);
	}

	// ----- lambda + 显式 thread* -----
	template <typename Recv, typename F,
		std::enable_if_t<
			std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
				!std::is_member_function_pointer_v<std::decay_t<F>>,
			int> = 0>
	connection connect(const sptr<Recv> &receiver, F &&func, thread *target,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		(void)unique;
		if (!receiver)
			return {};
		Recv *raw = receiver.get();
		wptr<Recv> weak = receiver;
		std::weak_ptr<void> life(
			std::static_pointer_cast<void>(sptr<Recv>(receiver)));
		return add_connection(raw, std::move(life), target,
			[weak, fn = std::decay_t<F>(std::forward<F>(func))](
				const Args &...args) mutable {
				auto locked = weak.lock();
				if (!locked)
					return;
				(void)fn(args...);
			},
			type, false, slot_key{});
	}

	// ----- lambda：无 method，target 为空（仅 Direct / Auto→Direct）-----
	template <typename Recv, typename F,
		std::enable_if_t<
			std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
				!std::is_member_function_pointer_v<std::decay_t<F>>,
			int> = 0>
	connection connect(const sptr<Recv> &receiver, F &&func,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		return connect(receiver, std::forward<F>(func), nullptr, type, unique);
	}

	template <typename Recv, typename SlotClass, typename R, typename... SlotArgs>
	connection connect(const wptr<Recv> &receiver,
		slots_t<R> (SlotClass::*method)(SlotArgs...), thread *target,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked, method, target, type, unique);
	}
	template <typename Recv, typename SlotClass, typename R, typename... SlotArgs>
	connection connect(const wptr<Recv> &receiver,
		slots_t<R> (SlotClass::*method)(SlotArgs...) const, thread *target,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked, method, target, type, unique);
	}
	template <typename Recv, typename SlotClass, typename R, typename... SlotArgs>
	connection connect(const wptr<Recv> &receiver,
		slots_t<R> (SlotClass::*method)(SlotArgs...),
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked, method, type, unique);
	}
	template <typename Recv, typename SlotClass, typename R, typename... SlotArgs>
	connection connect(const wptr<Recv> &receiver,
		slots_t<R> (SlotClass::*method)(SlotArgs...) const,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked, method, type, unique);
	}
	template <typename Recv, typename F,
		std::enable_if_t<
			std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
				!std::is_member_function_pointer_v<std::decay_t<F>>,
			int> = 0>
	connection connect(const wptr<Recv> &receiver, F &&func, thread *target,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked, std::forward<F>(func), target, type, unique);
	}
	template <typename Recv, typename F,
		std::enable_if_t<
			std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
				!std::is_member_function_pointer_v<std::decay_t<F>>,
			int> = 0>
	connection connect(const wptr<Recv> &receiver, F &&func,
		connection_type type = connection_type::automatic,
		bool unique = false) {
		auto locked = receiver.lock();
		return connect(locked, std::forward<F>(func), type, unique);
	}

	void disconnect_receiver(void *receiver) {
		if (!receiver)
			return;
		std::vector<std::shared_ptr<connection_state>> states;
		{
			std::lock_guard<std::mutex> lock(_ctl->mutex);
			auto cur = _ctl->load_list();
			auto draft = std::make_shared<entry_list>();
			draft->reserve(cur->size());
			for (const auto &e : *cur) {
				if (e._receiver == receiver) {
					if (e._state &&
						e._state->_alive.exchange(false,
							std::memory_order_acq_rel)) {
						states.push_back(e._state);
					}
					continue;
				}
				draft->push_back(e);
			}
			_ctl->store_list(std::move(draft));
		}
		for (auto &s : states) {
			if (s && s->_disconnect_fn)
				s->_disconnect_fn();
		}
	}

	/// @brief 连接成员函数,将成员函数绑定到信号上,并返回一个连接对象
	/// @param receiver 接收者对象
	/// @param method 成员函数
	/// @param target 目标线程
	/// @param type 连接类型
	/// @param unique 是否唯一
	/// @param bind_affinity 是否绑定亲和表,不绑定表示先查看亲和表,没有找到再emit线程运行
	/// @return 连接对象
	template <typename Recv, typename Method>
	connection connect_pmf_shared(const sptr<Recv> &receiver, Method method,
		thread *target, connection_type type, bool unique, bool bind_affinity) {
		if (!receiver || !method)
			return {};
		Recv *raw = receiver.get();
		if (bind_affinity && target)
			bind_slot_affinity(receiver, method, target);
		wptr<Recv> weak = receiver;
		std::weak_ptr<void> life(
			std::static_pointer_cast<void>(sptr<Recv>(receiver)));
		return add_connection(raw, life, target,
			[weak, method](const Args &...args) {
				auto locked = weak.lock();
				if (!locked)
					return;
				(void)((*locked).*method)(args...);
			},
			type, unique, make_pmf_key(raw, method));
	}

	struct slot_key {
		void *receiver = nullptr;
		const std::type_info *pmf_type = nullptr;
		std::shared_ptr<void> pmf;
		bool (*equal)(const void *, const void *) = nullptr;

		bool matches(const slot_key &other) const noexcept {
			if (receiver != other.receiver || !equal || !other.equal)
				return false;
			if (pmf_type != other.pmf_type || !pmf || !other.pmf)
				return false;
			return equal(pmf.get(), other.pmf.get());
		}
	};

	/// @brief 连接项,保存连接的详细信息,包括这个连接的接收者,槽函数,存活状态,目标线程等,连接方式
	struct entry {
		std::shared_ptr<connection_state> _state;
		void *_receiver = nullptr;
		std::weak_ptr<void> _receiver_alive;
		thread *_target = nullptr;
		std::weak_ptr<void> _target_alive;
		std::shared_ptr<slot> _slot;
		connection_type _type = connection_type::automatic;
		slot_key _key;
	};

	/// @brief 连接项列表,保存所有连接的详细信息
	using entry_list = std::vector<entry>;

	/// @brief 控制器,管理连接项列表,包括互斥锁,已发布列表,当前列表,垃圾列表,读者计数,下一个ID
	struct control {
		mutable std::mutex mutex;
		std::atomic<const entry_list *> published{nullptr};
		std::shared_ptr<const entry_list> current;
		std::vector<std::shared_ptr<const entry_list>> graveyard; // 垃圾列表,保存已废弃的连接项列表
		std::atomic<std::uint32_t> readers{0}; // 读者计数,用于管理连接项列表的并发访问
		std::uint64_t next_id = 1;

		control() {
			current = std::make_shared<const entry_list>();
			published.store(current.get(), std::memory_order_release);
		}

		std::shared_ptr<const entry_list> load_list() const { return current; }

		/// @brief 存储连接项列表,更新已发布列表,垃圾列表,读者计数
		void store_list(std::shared_ptr<entry_list> draft) {
			std::shared_ptr<const entry_list> next(std::move(draft));
			const entry_list *raw = next.get();
			auto old = std::move(current);
			current = std::move(next);
			published.store(raw, std::memory_order_release);
			if (old) {
				if (readers.load(std::memory_order_acquire) == 0) {
					old.reset();
				} else {
					graveyard.push_back(std::move(old));
				}
			}
			if (readers.load(std::memory_order_acquire) == 0 &&
				!graveyard.empty()) {
				graveyard.clear(); // 清除垃圾列表
			}
		}
	};

	template <class M>
	static slot_key make_pmf_key(void *receiver, M method) {
		slot_key key;
		key.receiver = receiver;
		key.pmf_type = &typeid(M);
		key.pmf = std::make_shared<M>(method);
		key.equal = [](const void *a, const void *b) {
			return *static_cast<const M *>(a) == *static_cast<const M *>(b);
		};
		return key;
	}

	connection add_connection(void *receiver, std::weak_ptr<void> lifetime,
		thread *target, slot slot_, connection_type type, bool unique,
		slot_key key) {
		if (!lifetime.lock())
			return {};

		auto state = std::make_shared<connection_state>();
		std::uint64_t id = 0;
		{
			std::lock_guard<std::mutex> lock(_ctl->mutex);
			auto cur = _ctl->load_list();
			if (unique && key.equal) {
				for (const auto &e : *cur) {
					if (e._state &&
						e._state->_alive.load(std::memory_order_acquire) &&
						e._key.matches(key)) {
						return {};
					}
				}
			}
			id = _ctl->next_id++;
			state->_id = id;
			std::weak_ptr<control> wctl = _ctl;
			state->_disconnect_fn = [wctl, id]() {
				if (auto ctl = wctl.lock()) {
					std::lock_guard<std::mutex> lock(ctl->mutex);
					auto cur = ctl->load_list();
					auto draft = std::make_shared<entry_list>();
					draft->reserve(cur->size());
					for (const auto &e : *cur) {
						if (e._state && e._state->_id == id)
							continue;
						draft->push_back(e);
					}
					ctl->store_list(std::move(draft));
				}
			};

			auto draft = std::make_shared<entry_list>(*cur);
			entry e;
			e._state = state;
			e._receiver = receiver;
			e._receiver_alive = std::move(lifetime);
			e._target = target;
			if (target)
				e._target_alive = target->identity();
			e._slot = std::make_shared<slot>(std::move(slot_));
			e._type = type;
			e._key = std::move(key);
			draft->push_back(std::move(e));
			_ctl->store_list(std::move(draft));
		}

		return connection{std::move(state)};
	}

	std::shared_ptr<control> _ctl = std::make_shared<control>();
	std::atomic<bool> _blocked{false};
};

// =============================================================================
// invoke 路由与实现
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

struct invoke_route {
	bool use_direct = false;
	bool use_blocking = false;
	std::errc error = std::errc{};
	bool failed() const noexcept { return error != std::errc{}; }
};

/// same_thread = (current_thread() == target)；无 target 时 Auto→Direct，Queued 失败
inline invoke_route resolve_invoke_route(thread *target, connection_type type) {
	invoke_route r;
	const bool same_thread = (target != nullptr && target == current_thread());

	if (type == connection_type::queued) {
		if (!target) {
			r.error = std::errc::operation_not_permitted;
			return r;
		}
		r.error = std::errc::operation_in_progress;
		return r;
	}
	if (type == connection_type::blocking_queued) {
		if (!target) {
			r.error = std::errc::operation_not_permitted;
			return r;
		}
		if (same_thread) {
			r.use_direct = true;
		} else {
			auto loop_ = target->loop_shared();
			if (!loop_ || !loop_->is_pumping())
				r.error = std::errc::operation_not_permitted;
			else
				r.use_blocking = true;
		}
		return r;
	}
	if (type == connection_type::direct) {
		r.use_direct = true;
		return r;
	}
	// automatic：无绑定或同线程 → Direct；跨线程 → 异步（无同步 result）
	if (!target || same_thread) {
		r.use_direct = true;
	} else {
		r.error = std::errc::operation_in_progress;
	}
	return r;
}

template <class Recv, class Method, class... Args>
result<typename slot_pmf_traits<Method>::value_type>
invoke_slot(const sptr<Recv> &receiver, Method method, thread *target,
	connection_type type, Args &&...args) {
	using R = typename slot_pmf_traits<Method>::value_type;
	if (!receiver || !method)
		return err(std::errc::invalid_argument);

	const invoke_route route = resolve_invoke_route(target, type);
	if (route.failed())
		return err(route.error);

	wptr<Recv> weak = receiver;

	auto call = [&]() -> result<R> {
		auto locked = weak.lock();
		if (!locked)
			return err(std::errc::owner_dead);
		if constexpr (std::is_void_v<R>) {
			((*locked).*method)(std::forward<Args>(args)...);
			return {};
		} else {
			return ((*locked).*method)(std::forward<Args>(args)...).get();
		}
	};

	if (route.use_direct)
		return call();
	if (!route.use_blocking)
		return err(std::errc::operation_in_progress);

	auto loop_ = target->loop_shared();
	if (!loop_)
		return err(std::errc::operation_not_permitted);
	auto out =
		std::make_shared<result<R>>(err(std::errc::operation_canceled));
	std::tuple<std::decay_t<Args>...> bound_args{std::forward<Args>(args)...};
	const bool posted = loop_->post_blocking(
		[weak, method, bound_args = std::move(bound_args), out]() mutable {
			auto locked = weak.lock();
			if (!locked) {
				*out = result<R>(err(std::errc::owner_dead));
				return;
			}
			if constexpr (std::is_void_v<R>) {
				std::apply(
					[&](auto &&...a) {
						((*locked).*method)(std::forward<decltype(a)>(a)...);
					},
					std::move(bound_args));
				*out = result<R>{};
			} else {
				*out = std::apply(
					[&](auto &&...a) {
						return ((*locked).*method)(
							std::forward<decltype(a)>(a)...)
							.get();
					},
					std::move(bound_args));
			}
		});
	if (!posted)
		return err(std::errc::operation_not_permitted);
	return std::move(*out);
}

template <class F, class... Args>
using invoke_callable_result_t =
	std::decay_t<std::invoke_result_t<std::decay_t<F> &, Args...>>;

template <class F, class... Args>
result<invoke_callable_result_t<F, Args...>>
invoke_callable(thread *target, connection_type type, F &&fn, Args &&...args) {
	using R = invoke_callable_result_t<F, Args...>;
	const invoke_route route = resolve_invoke_route(target, type);
	if (route.failed())
		return err(route.error);

	auto fn_store = std::decay_t<F>(std::forward<F>(fn));

	auto call = [&]() -> result<R> {
		if constexpr (std::is_void_v<R>) {
			std::invoke(fn_store, std::forward<Args>(args)...);
			return {};
		} else {
			return std::invoke(fn_store, std::forward<Args>(args)...);
		}
	};

	if (route.use_direct)
		return call();
	if (!route.use_blocking)
		return err(std::errc::operation_in_progress);

	auto loop_ = target->loop_shared();
	if (!loop_)
		return err(std::errc::operation_not_permitted);
	auto out =
		std::make_shared<result<R>>(err(std::errc::operation_canceled));
	std::tuple<std::decay_t<Args>...> bound_args{std::forward<Args>(args)...};
	const bool posted = loop_->post_blocking(
		[fn_store = std::move(fn_store), bound_args = std::move(bound_args),
			out]() mutable {
			if constexpr (std::is_void_v<R>) {
				std::apply(
					[&](auto &&...a) {
						std::invoke(fn_store, std::forward<decltype(a)>(a)...);
					},
					std::move(bound_args));
				*out = result<R>{};
			} else {
				*out = std::apply(
					[&](auto &&...a) {
						return std::invoke(fn_store,
							std::forward<decltype(a)>(a)...);
					},
					std::move(bound_args));
			}
		});
	if (!posted)
		return err(std::errc::operation_not_permitted);
	return std::move(*out);
}

} // namespace detail

// ----- 成员槽 invoke：显式 thread* -----
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const sptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), thread *target, connection_type type,
	SlotArgs... args) {
	return detail::invoke_slot(receiver, method, target, type,
		std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const sptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...) const, thread *target,
	connection_type type, SlotArgs... args) {
	return detail::invoke_slot(receiver, method, target, type,
		std::forward<SlotArgs>(args)...);
}

// ----- 成员槽 invoke：查亲和表 -----
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const sptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), connection_type type,
	SlotArgs... args) {
	thread *target = find_slot_affinity(receiver, method);
	return detail::invoke_slot(receiver, method, target, type,
		std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const sptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...) const, connection_type type,
	SlotArgs... args) {
	thread *target = find_slot_affinity(receiver, method);
	return detail::invoke_slot(receiver, method, target, type,
		std::forward<SlotArgs>(args)...);
}

template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const sptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), SlotArgs... args) {
	return invoke(receiver, method, connection_type::automatic,
		std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const sptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...) const, SlotArgs... args) {
	return invoke(receiver, method, connection_type::automatic,
		std::forward<SlotArgs>(args)...);
}

template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const wptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), thread *target, connection_type type,
	SlotArgs... args) {
	auto locked = receiver.lock();
	if (!locked)
		return err(std::errc::owner_dead);
	return invoke(locked, method, target, type,
		std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const wptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...) const, thread *target,
	connection_type type, SlotArgs... args) {
	auto locked = receiver.lock();
	if (!locked)
		return err(std::errc::owner_dead);
	return invoke(locked, method, target, type,
		std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const wptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), connection_type type,
	SlotArgs... args) {
	auto locked = receiver.lock();
	if (!locked)
		return err(std::errc::owner_dead);
	return invoke(locked, method, type, std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const wptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...) const, connection_type type,
	SlotArgs... args) {
	auto locked = receiver.lock();
	if (!locked)
		return err(std::errc::owner_dead);
	return invoke(locked, method, type, std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const wptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...), SlotArgs... args) {
	auto locked = receiver.lock();
	if (!locked)
		return err(std::errc::owner_dead);
	return invoke(locked, method, std::forward<SlotArgs>(args)...);
}
template <class Recv, class C, class R, class... SlotArgs>
result<R> invoke(const wptr<Recv> &receiver,
	slots_t<R> (C::*method)(SlotArgs...) const, SlotArgs... args) {
	auto locked = receiver.lock();
	if (!locked)
		return err(std::errc::owner_dead);
	return invoke(locked, method, std::forward<SlotArgs>(args)...);
}

/**
 * @brief 在显式 thread 上调用可调用对象并取回返回值。
 * @note Queued / 跨线程 Auto 无法同步取值 → operation_in_progress。
 */
template <class F, class... Args,
	std::enable_if_t<std::is_invocable_v<std::decay_t<F> &, Args...> &&
						 !std::is_member_pointer_v<std::decay_t<F>>,
		int> = 0>
auto invoke(thread *target, connection_type type, F &&fn, Args &&...args)
	-> result<detail::invoke_callable_result_t<F, Args...>> {
	return detail::invoke_callable(target, type, std::forward<F>(fn),
		std::forward<Args>(args)...);
}

/// 把无返回值任务投递到显式 thread（fire-and-forget）
inline void invoke(thread *target, std::function<void()> fn,
	connection_type type = connection_type::automatic) {
	if (!fn)
		return;

	const detail::invoke_route route = detail::resolve_invoke_route(target, type);
	if (route.use_direct) {
		fn();
		return;
	}
	if (!target)
		return;
	auto loop_ = target->loop_shared();
	if (!loop_)
		return;
	if (route.use_blocking) {
		(void)loop_->post_blocking(std::move(fn));
	} else if (type == connection_type::queued ||
			   type == connection_type::automatic) {
		(void)loop_->post(std::move(fn));
	}
}

/// @brief 连接信号和槽函数,将槽函数绑定到信号上,并返回一个连接对象
/// @param signal_ 信号对象
/// @param receiver 接收者对象
/// @param method 槽函数
/// @param target 目标线程
/// @param type 连接类型
/// @param unique 是否唯一
/// @return 连接对象
template <typename... Args, typename Recv, typename SlotClass, typename R,
	typename... SlotArgs>
connection connect(signal<Args...> &signal_, const sptr<Recv> &receiver,
	slots_t<R> (SlotClass::*method)(SlotArgs...), thread *target,
	connection_type type = connection_type::automatic, bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver, method, target,
		type, unique);
}
template <typename... Args, typename Recv, typename SlotClass, typename R,
	typename... SlotArgs>
connection connect(signal<Args...> &signal_, const sptr<Recv> &receiver,
	slots_t<R> (SlotClass::*method)(SlotArgs...) const, thread *target,
	connection_type type = connection_type::automatic, bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver, method, target,
		type, unique);
}
template <typename... Args, typename Recv, typename SlotClass, typename R,
	typename... SlotArgs>
connection connect(signal<Args...> &signal_, const sptr<Recv> &receiver,
	slots_t<R> (SlotClass::*method)(SlotArgs...),
	connection_type type = connection_type::automatic, bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver, method, type,
		unique);
}
template <typename... Args, typename Recv, typename SlotClass, typename R,
	typename... SlotArgs>
connection connect(signal<Args...> &signal_, const sptr<Recv> &receiver,
	slots_t<R> (SlotClass::*method)(SlotArgs...) const,
	connection_type type = connection_type::automatic, bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver, method, type,
		unique);
}

template <typename... Args, typename Recv, typename SlotClass, typename R,
	typename... SlotArgs>
connection connect(signal<Args...> &signal_, const wptr<Recv> &receiver,
	slots_t<R> (SlotClass::*method)(SlotArgs...), thread *target,
	connection_type type = connection_type::automatic, bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver, method, target,
		type, unique);
}
template <typename... Args, typename Recv, typename SlotClass, typename R,
	typename... SlotArgs>
connection connect(signal<Args...> &signal_, const wptr<Recv> &receiver,
	slots_t<R> (SlotClass::*method)(SlotArgs...) const, thread *target,
	connection_type type = connection_type::automatic, bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver, method, target,
		type, unique);
}
template <typename... Args, typename Recv, typename SlotClass, typename R,
	typename... SlotArgs>
connection connect(signal<Args...> &signal_, const wptr<Recv> &receiver,
	slots_t<R> (SlotClass::*method)(SlotArgs...),
	connection_type type = connection_type::automatic, bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver, method, type,
		unique);
}
template <typename... Args, typename Recv, typename SlotClass, typename R,
	typename... SlotArgs>
connection connect(signal<Args...> &signal_, const wptr<Recv> &receiver,
	slots_t<R> (SlotClass::*method)(SlotArgs...) const,
	connection_type type = connection_type::automatic, bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver, method, type,
		unique);
}

template <typename... Args, typename Recv, typename F,
	std::enable_if_t<
		std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
			!std::is_member_function_pointer_v<std::decay_t<F>>,
		int> = 0>
connection connect(signal<Args...> &signal_, const sptr<Recv> &receiver,
	F &&func, thread *target, connection_type type = connection_type::automatic,
	bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver,
		std::forward<F>(func), target, type, unique);
}
template <typename... Args, typename Recv, typename F,
	std::enable_if_t<
		std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
			!std::is_member_function_pointer_v<std::decay_t<F>>,
		int> = 0>
connection connect(signal<Args...> &signal_, const sptr<Recv> &receiver,
	F &&func, connection_type type = connection_type::automatic,
	bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver,
		std::forward<F>(func), type, unique);
}
template <typename... Args, typename Recv, typename F,
	std::enable_if_t<
		std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
			!std::is_member_function_pointer_v<std::decay_t<F>>,
		int> = 0>
connection connect(signal<Args...> &signal_, const wptr<Recv> &receiver,
	F &&func, thread *target, connection_type type = connection_type::automatic,
	bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver,
		std::forward<F>(func), target, type, unique);
}
template <typename... Args, typename Recv, typename F,
	std::enable_if_t<
		std::is_invocable_v<std::decay_t<F> &, const Args &...> &&
			!std::is_member_function_pointer_v<std::decay_t<F>>,
		int> = 0>
connection connect(signal<Args...> &signal_, const wptr<Recv> &receiver,
	F &&func, connection_type type = connection_type::automatic,
	bool unique = false) {
	return detail::signal_connecter::connect(signal_, receiver,
		std::forward<F>(func), type, unique);
}

} // namespace utils

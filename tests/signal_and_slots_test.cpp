#include "concurrency/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout =
                               std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct sender {
    utils::signal<int> value_changed;
};

struct receiver {
    mutable std::atomic<int> last{0};
    int calls = 0;

    utils::slots_t<> on_value(int value) {
        last = value;
        ++calls;
        return {};
    }

    utils::slots_t<> on_value_const(int value) const {
        last = value;
        return {};
    }

    utils::slots_t<int> twice(int value) { return value * 2; }

    utils::slots_t<int> twice_const(int value) const { return value * 2; }
};

} // namespace

// =============================================================================
// slots_t
// =============================================================================

TEST(SlotsT, ValueConstructConvertAndGet) {
    utils::slots_t<int> s(42);
    EXPECT_EQ(static_cast<int>(s), 42);
    EXPECT_EQ(s.get(), 42);
}

TEST(SlotsT, VoidSpecialization) { utils::slots_t<> v; (void)v; }


/// =============================================================================
/// method_key
/// =============================================================================

class method_key_test {
public:
    utils::slots_t<> test_method() {
        return {};
    }

    utils::slots_t<> test_method2() {
        return {};
    }
};


TEST(MethodKey, ConstructorAndEquality) {
    utils::detail::method_key mk1;
    utils::detail::method_key mk2;
    utils::detail::method_key mk3;
    mk1.type = std::type_index(typeid(utils::slots_t<>(method_key_test::*)()));
    auto pmf = &method_key_test::test_method;
    std::memcpy(mk1.bytes.data(), &pmf, sizeof(pmf));
    mk1.size = static_cast<std::uint8_t>(sizeof(pmf));

    mk2.type = std::type_index(typeid(utils::slots_t<>(method_key_test::*)()));
    auto pmf2 = &method_key_test::test_method;
    std::memcpy(mk2.bytes.data(), &pmf2, sizeof(pmf2));
    mk2.size = static_cast<std::uint8_t>(sizeof(pmf2)); 

    mk3.type = std::type_index(typeid(utils::slots_t<>(method_key_test::*)()));
    auto pmf3 = &method_key_test::test_method2;
    std::memcpy(mk3.bytes.data(), &pmf3, sizeof(pmf3));
    mk3.size = static_cast<std::uint8_t>(sizeof(pmf3));

    EXPECT_EQ(mk1, mk2);
    EXPECT_NE(mk1, mk3);
    EXPECT_NE(mk2, mk3);
}

TEST(MethodKey, MakeMethodKey) {
    utils::detail::method_key mk = utils::detail::make_method_key(&method_key_test::test_method);
    utils::detail::method_key mk2 = utils::detail::make_method_key(&method_key_test::test_method);
    utils::detail::method_key mk3 = utils::detail::make_method_key(&method_key_test::test_method2);
    EXPECT_EQ(mk, mk2);
    EXPECT_NE(mk, mk3);
    EXPECT_NE(mk2, mk3);
}

// =============================================================================
// aliases
// =============================================================================

TEST(SmartPtrAliases, SptrUptrWptr) {
    static_assert(std::is_same_v<utils::sptr<int>, std::shared_ptr<int>>);
    static_assert(std::is_same_v<utils::uptr<int>, std::unique_ptr<int>>);
    static_assert(std::is_same_v<utils::wptr<int>, std::weak_ptr<int>>);

    utils::sptr<int> s = std::make_shared<int>(1);
    utils::wptr<int> w = s;
    utils::uptr<int> u = std::make_unique<int>(2);
    EXPECT_EQ(*s, 1);
    EXPECT_EQ(*u, 2);
    EXPECT_FALSE(w.expired());
}

// =============================================================================
// event_loop（公开 API 全覆盖）
// =============================================================================

TEST(EventLoop, PostAndProcessEvents) {
    utils::event_loop loop;
    int value = 0;
    EXPECT_TRUE(loop.post([&] { value = 1; }));
    loop.process_events();
    EXPECT_EQ(value, 1);
}

TEST(EventLoop, PostEmptyTaskReturnsFalse) {
    utils::event_loop loop;
    EXPECT_FALSE(loop.post(nullptr));
    EXPECT_FALSE(loop.post({}));
}

TEST(EventLoop, PostAfterStopReturnsFalse) {
    utils::event_loop loop;
    loop.stop();
    EXPECT_FALSE(loop.is_running());
    EXPECT_FALSE(loop.post([] {}));
}

TEST(EventLoop, SetAcceptingReenablesPostAfterStop) {
    utils::event_loop loop;
    loop.stop();
    EXPECT_FALSE(loop.post([] {}));
    loop.set_accepting(true);
    EXPECT_TRUE(loop.is_running());
    int value = 0;
    EXPECT_TRUE(loop.post([&] { value = 7; }));
    loop.process_events();
    EXPECT_EQ(value, 7);
}

TEST(EventLoop, ProcessEventsWithBudgetWaitsForDelayed) {
    utils::event_loop loop;
    int value = 0;
    EXPECT_NE(loop.post_delayed(std::chrono::milliseconds(20),
                                [&] { value = 3; }),
              0u);
    loop.process_events(std::chrono::milliseconds(200));
    EXPECT_EQ(value, 3);
}

TEST(EventLoop, ProcessEventsSwallowsTaskException) {
    utils::event_loop loop;
    int value = 0;
    EXPECT_TRUE(loop.post([] { throw std::runtime_error("boom"); }));
    EXPECT_TRUE(loop.post([&] { value = 1; }));
    EXPECT_NO_THROW(loop.process_events());
    EXPECT_EQ(value, 1);
}

TEST(EventLoop, IsPumpingDuringProcessEvents) {
    utils::event_loop loop;
    EXPECT_FALSE(loop.is_pumping());
    EXPECT_FALSE(loop.is_pumping_on_current_thread());
    bool saw_pumping = false;
    bool saw_on_current = false;
    EXPECT_TRUE(loop.post([&] {
        saw_pumping = loop.is_pumping();
        saw_on_current = loop.is_pumping_on_current_thread();
    }));
    loop.process_events();
    EXPECT_TRUE(saw_pumping);
    EXPECT_TRUE(saw_on_current);
    EXPECT_FALSE(loop.is_pumping());
    EXPECT_FALSE(loop.is_pumping_on_current_thread());
}

TEST(EventLoop, PostBlockingFromOtherThreadSucceeds) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));
    std::atomic<int> value{0};
    EXPECT_TRUE(worker.loop()->post_blocking([&] { value = 42; }));
    EXPECT_EQ(value.load(), 42);
    worker.stop();
}

TEST(EventLoop, PostBlockingFailsWhenNotPumping) {
    utils::event_loop loop;
    EXPECT_FALSE(loop.is_pumping());
    EXPECT_FALSE(loop.post_blocking([] {}));
}

TEST(EventLoop, PostBlockingEmptyTaskReturnsFalse) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));
    EXPECT_FALSE(worker.loop()->post_blocking(nullptr));
    worker.stop();
}

TEST(EventLoop, PostBlockingRejectsSelfDeadlock) {
    utils::event_loop loop;
    bool rejected = false;
    EXPECT_TRUE(loop.post([&] {
        rejected = !loop.post_blocking([] {});
    }));
    loop.process_events();
    EXPECT_TRUE(rejected);
}

TEST(EventLoop, PostBlockingRethrowsTaskException) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));
    EXPECT_THROW(worker.loop()->post_blocking(
                     [] { throw std::runtime_error("blocking boom"); }),
                 std::runtime_error);
    worker.stop();
}

TEST(EventLoop, PostDelayedZeroActsAsPost) {
    utils::event_loop loop;
    int value = 0;
    EXPECT_EQ(loop.post_delayed(std::chrono::milliseconds(0),
                                [&] { value = 9; }),
              0u);
    loop.process_events();
    EXPECT_EQ(value, 9);
}

TEST(EventLoop, PostDelayedEmptyOrStoppedReturnsZero) {
    utils::event_loop loop;
    EXPECT_EQ(loop.post_delayed(std::chrono::milliseconds(10), nullptr), 0u);
    loop.stop();
    EXPECT_EQ(loop.post_delayed(std::chrono::milliseconds(10), [] {}), 0u);
}

TEST(EventLoop, PostDelayedFiresAfterDelay) {
    utils::event_loop loop;
    int value = 0;
    const auto id =
        loop.post_delayed(std::chrono::milliseconds(30), [&] { value = 5; });
    EXPECT_NE(id, 0u);
    loop.process_events();
    EXPECT_EQ(value, 0);
    loop.process_events(std::chrono::milliseconds(200));
    EXPECT_EQ(value, 5);
}

TEST(EventLoop, CancelTimerPreventsDelayedTask) {
    utils::event_loop loop;
    int value = 0;
    const auto id =
        loop.post_delayed(std::chrono::milliseconds(30), [&] { value = 1; });
    ASSERT_NE(id, 0u);
    loop.cancel_timer(id);
    loop.cancel_timer(0); // no-op
    loop.process_events(std::chrono::milliseconds(200));
    EXPECT_EQ(value, 0);
}

TEST(EventLoop, PostPeriodicInvalidArgsReturnZero) {
    utils::event_loop loop;
    EXPECT_EQ(loop.post_periodic(std::chrono::milliseconds(10), nullptr), 0u);
    EXPECT_EQ(loop.post_periodic(std::chrono::milliseconds(0), [] {}), 0u);
    loop.stop();
    EXPECT_EQ(loop.post_periodic(std::chrono::milliseconds(10), [] {}), 0u);
}

TEST(EventLoop, PostPeriodicFiresMultipleTimesUntilCancelled) {
    utils::event_loop loop;
    std::atomic<int> count{0};
    const auto id = loop.post_periodic(std::chrono::milliseconds(25),
                                       [&] { count.fetch_add(1); });
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(wait_until(
        [&] {
            loop.process_events(std::chrono::milliseconds(40));
            return count.load() >= 2;
        },
        std::chrono::milliseconds(1000)));
    loop.cancel_timer(id);
    const int after_cancel = count.load();
    loop.process_events(std::chrono::milliseconds(80));
    EXPECT_LE(count.load(), after_cancel + 1);
}

TEST(EventLoop, RunProcessesPostedTaskThenStop) {
    utils::event_loop loop;
    std::atomic<int> value{0};
    std::thread runner([&] { loop.run(); });
    ASSERT_TRUE(wait_until([&] { return loop.is_pumping(); }));
    EXPECT_TRUE(loop.post([&] { value = 11; }));
    ASSERT_TRUE(wait_until([&] { return value.load() == 11; }));
    loop.stop();
    runner.join();
    EXPECT_FALSE(loop.is_running());
}

TEST(EventLoop, StopWakesRun) {
    utils::event_loop loop;
    std::thread runner([&] { loop.run(); });
    ASSERT_TRUE(wait_until([&] { return loop.is_pumping(); }));
    loop.stop();
    runner.join();
    EXPECT_FALSE(loop.is_running());
}

TEST(EventLoop, IsRunningDefaultsTrue) {
    utils::event_loop loop;
    EXPECT_TRUE(loop.is_running());
    loop.stop();
    EXPECT_FALSE(loop.is_running());
}

// =============================================================================
// thread（公开 API）
// =============================================================================

TEST(Thread, CurrentThreadNullOnMain) {
    EXPECT_EQ(utils::current_thread(), nullptr);
}

TEST(Thread, WorkerThreadAliasIsSameType) {
    static_assert(std::is_same_v<utils::worker_thread, utils::thread>);
}

TEST(Thread, LoopNullBeforeStart) {
    utils::thread worker;
    EXPECT_EQ(worker.loop(), nullptr);
    EXPECT_EQ(worker.loop_shared(), nullptr);
    EXPECT_FALSE(worker.is_running());
}

TEST(Thread, StartStop) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    ASSERT_NE(worker.loop(), nullptr);
    ASSERT_NE(worker.loop_shared(), nullptr);
    worker.stop();
    EXPECT_FALSE(worker.is_running());
    EXPECT_EQ(worker.loop(), nullptr);
    EXPECT_EQ(worker.loop_shared(), nullptr);
}

TEST(Thread, StartIsIdempotent) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    auto *loop1 = worker.loop();
    worker.start(); // already running → no-op
    EXPECT_EQ(worker.loop(), loop1);
    EXPECT_TRUE(worker.is_running());
    worker.stop();
}

TEST(Thread, RestartAfterStop) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    worker.stop();
    EXPECT_FALSE(worker.is_running());

    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    std::atomic<int> value{0};
    EXPECT_TRUE(worker.loop()->post([&] { value = 5; }));
    ASSERT_TRUE(wait_until([&] { return value.load() == 5; }));
    worker.stop();
}

TEST(Thread, StopWithoutStartIsNoop) {
    utils::thread worker;
    EXPECT_NO_THROW(worker.stop());
    EXPECT_FALSE(worker.is_running());
}

TEST(Thread, LoopSharedKeepsLoopAlive) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    auto held = worker.loop_shared();
    ASSERT_NE(held, nullptr);
    worker.stop();
    EXPECT_EQ(worker.loop(), nullptr);
    // 外部仍持有 shared_ptr，loop 对象可继续 process_events
    int value = 0;
    held->set_accepting(true);
    EXPECT_TRUE(held->post([&] { value = 1; }));
    held->process_events();
    EXPECT_EQ(value, 1);
}

TEST(Thread, CurrentThreadPointsToWorkerInsideLoop) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    utils::thread *seen = nullptr;
    EXPECT_TRUE(worker.loop()->post_blocking([&] {
        seen = utils::current_thread();
    }));
    EXPECT_EQ(seen, &worker);
    worker.stop();
    EXPECT_EQ(utils::current_thread(), nullptr);
}

TEST(Thread, IdentityWeakRemainsValidWhileThreadObjectLives) {
    utils::thread worker;
    auto id = worker.identity();
    EXPECT_FALSE(id.expired());
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    EXPECT_FALSE(id.expired());
    worker.stop();
    EXPECT_FALSE(id.expired());
}

TEST(Thread, IdentityExpiresAfterThreadDestroyed) {
    std::weak_ptr<void> id;
    {
        utils::thread worker;
        id = worker.identity();
        EXPECT_FALSE(id.expired());
    }
    EXPECT_TRUE(id.expired());
}

TEST(Thread, DestructorStopsRunningWorker) {
    std::atomic<bool> ran{false};
    {
        utils::thread worker;
        worker.start();
        ASSERT_TRUE(wait_until([&] {
            return worker.loop() && worker.loop()->is_pumping();
        }));
        EXPECT_TRUE(worker.loop()->post([&] { ran = true; }));
        ASSERT_TRUE(wait_until([&] { return ran.load(); }));
    } // dtor → stop + join
    SUCCEED();
}

TEST(Thread, PostViaLoopWhileRunning) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    std::atomic<int> sum{0};
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(worker.loop()->post([&] { sum.fetch_add(1); }));
    }
    ASSERT_TRUE(wait_until([&] { return sum.load() == 10; }));
    worker.stop();
}

TEST(Thread, StopFromOwnThreadIsRejected) {
#ifdef NDEBUG
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    // Release：本线程 stop 会 assert 路径 return，不 join；之后主线程仍可 stop
    EXPECT_TRUE(worker.loop()->post_blocking([&] { worker.stop(); }));
    EXPECT_TRUE(worker.is_running());
    worker.stop();
    EXPECT_FALSE(worker.is_running());
#else
    GTEST_SKIP() << "Debug build hits assert in thread::stop() from own thread";
#endif
}

// =============================================================================
// slot affinity
// =============================================================================

TEST(SlotAffinity, BindFindViaSptr) {
    auto rp = std::make_shared<receiver>();
    utils::thread worker;
    EXPECT_EQ(utils::find_slot_affinity(rp.get(), &receiver::on_value), nullptr);
    utils::bind_slot_affinity(rp.get(), &receiver::on_value, &worker);
    EXPECT_EQ(utils::find_slot_affinity(rp.get(), &receiver::on_value), &worker);
    utils::clear_slot_affinity(rp.get());
    EXPECT_EQ(utils::find_slot_affinity(rp.get(), &receiver::on_value), nullptr);
}

TEST(SlotAffinity, BindFindClearViaSmartPointers) {
    auto rp = std::make_shared<receiver>();
    utils::wptr<receiver> wp = rp;
    utils::thread worker;
    utils::bind_slot_affinity(rp, &receiver::on_value, &worker);
    EXPECT_EQ(utils::find_slot_affinity(rp, &receiver::on_value), &worker);
    EXPECT_EQ(utils::find_slot_affinity(wp, &receiver::on_value), &worker);
    utils::clear_slot_affinity(rp);
    EXPECT_EQ(utils::find_slot_affinity(rp, &receiver::on_value), nullptr);

    utils::bind_slot_affinity(wp, &receiver::twice, &worker);
    EXPECT_EQ(utils::find_slot_affinity(wp, &receiver::twice), &worker);
    utils::clear_slot_affinity(wp);
    EXPECT_EQ(utils::find_slot_affinity(rp.get(), &receiver::twice), nullptr);
}

TEST(SlotAffinity, BindOverwriteAndExpiredWptr) {
    auto rp = std::make_shared<receiver>();
    utils::thread w1;
    utils::thread w2;
    utils::bind_slot_affinity(rp, &receiver::on_value, &w1);
    utils::bind_slot_affinity(rp, &receiver::on_value, &w2);
    EXPECT_EQ(utils::find_slot_affinity(rp, &receiver::on_value), &w2);

    utils::wptr<receiver> dead;
    utils::bind_slot_affinity(dead, &receiver::on_value, &w1); // no-op
    EXPECT_EQ(utils::find_slot_affinity(rp, &receiver::on_value), &w2);
}

TEST(SlotAffinity, FindNullAfterReceiverDestroyed) {
    utils::thread worker;
    const void *raw = nullptr;
    {
        auto rp = std::make_shared<receiver>();
        raw = rp.get();
        utils::bind_slot_affinity(rp, &receiver::on_value, &worker);
        EXPECT_EQ(utils::find_slot_affinity(raw, &receiver::on_value), &worker);
    }
    EXPECT_EQ(utils::find_slot_affinity(raw, &receiver::on_value), nullptr);
}

TEST(SlotAffinity, FindNullAfterTargetDestroyed) {
    auto rp = std::make_shared<receiver>();
    {
        utils::thread worker;
        utils::bind_slot_affinity(rp, &receiver::on_value, &worker);
        EXPECT_EQ(utils::find_slot_affinity(rp, &receiver::on_value), &worker);
    }
    EXPECT_EQ(utils::find_slot_affinity(rp, &receiver::on_value), nullptr);
}

// =============================================================================
// connection / signal via free connect only
// =============================================================================

TEST(Connection, DefaultAndDisconnect) {
    utils::connection c;
    EXPECT_FALSE(c.connected());

    sender s;
    auto r = std::make_shared<receiver>();
    auto conn = utils::connect(s.value_changed, r, &receiver::on_value);
    EXPECT_TRUE(conn.connected());
    conn.disconnect();
    EXPECT_FALSE(conn.connected());
}

TEST(ScopedConnection, RaII) {
    sender s;
    auto r = std::make_shared<receiver>();
    {
        utils::scoped_connection scoped(
            utils::connect(s.value_changed, r, &receiver::on_value,
                           utils::connection_type::direct));
        s.value_changed.emit(4);
        EXPECT_EQ(r->last, 4);
    }
    s.value_changed.emit(5);
    EXPECT_EQ(r->last, 4);
}

TEST(Signal, DirectEmitViaFreeConnect) {
    sender s;
    auto r = std::make_shared<receiver>();
    auto conn = utils::connect(s.value_changed, r, &receiver::on_value,
                               utils::connection_type::direct);
    s.value_changed.emit(5);
    EXPECT_EQ(r->last, 5);
    s.value_changed(9);
    EXPECT_EQ(r->last, 9);
    conn.disconnect();
    s.value_changed.emit(6);
    EXPECT_EQ(r->last, 9);
}

TEST(Signal, UniqueConnection) {
    sender s;
    auto r = std::make_shared<receiver>();
    auto first = utils::connect(s.value_changed, r, &receiver::on_value,
                                utils::connection_type::direct,
                                utils::unique_connection);
    auto second = utils::connect(s.value_changed, r, &receiver::on_value,
                                 utils::connection_type::direct,
                                 utils::unique_connection);
    EXPECT_TRUE(first.connected());
    EXPECT_FALSE(second.connected());
    s.value_changed.emit(1);
    EXPECT_EQ(r->calls, 1);
}

TEST(Signal, BlockSignals) {
    sender s;
    auto r = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r, &receiver::on_value,
                         utils::connection_type::direct);
    s.value_changed.block_signals(true);
    s.value_changed.emit(1);
    EXPECT_EQ(r->calls, 0);
    s.value_changed.block_signals(false);
    s.value_changed.emit(2);
    EXPECT_EQ(r->last, 2);
}

TEST(Signal, DisconnectBySptrAndAll) {
    sender s;
    auto r1 = std::make_shared<receiver>();
    auto r2 = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r1, &receiver::on_value,
                         utils::connection_type::direct);
    (void)utils::connect(s.value_changed, r2, &receiver::on_value,
                         utils::connection_type::direct);
    s.value_changed.disconnect(r1);
    s.value_changed.emit(1);
    EXPECT_EQ(r1->calls, 0);
    EXPECT_EQ(r2->calls, 1);
    s.value_changed.disconnect_all();
    s.value_changed.emit(2);
    EXPECT_EQ(r2->calls, 1);
}

TEST(Signal, LambdaAndWptr) {
    sender s;
    auto r = std::make_shared<receiver>();
    utils::wptr<receiver> wp = r;
    int captured = 0;
    utils::scoped_connection c1{utils::connect(
        s.value_changed, r, [&](int v) { captured = v; },
        utils::connection_type::direct)};
    utils::scoped_connection c2{utils::connect(
        s.value_changed, wp, &receiver::on_value,
        utils::connection_type::direct)};
    s.value_changed.emit(8);
    EXPECT_EQ(captured, 8);
    EXPECT_EQ(r->last.load(), 8);
}

TEST(Signal, ReceiverDestroyedSkipsQueued) {
    sender s;
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    {
        auto r = std::make_shared<receiver>();
        (void)utils::connect(s.value_changed, r, &receiver::on_value, &worker,
                             utils::connection_type::queued);
        // r 析构，连接内 weak 失效
    }
    s.value_changed.emit(1); // 不应崩溃
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    worker.stop();
}

TEST(Signal, QueuedAndBlockingOnWorker) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    sender s;
    auto r = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r, &receiver::on_value, &worker,
                         utils::connection_type::queued);
    s.value_changed.emit(42);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 42; }));

    auto r2 = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r2, &receiver::on_value, &worker,
                         utils::connection_type::blocking_queued);
    s.value_changed.emit(7);
    EXPECT_EQ(r2->last.load(), 7);
    worker.stop();
}

TEST(Signal, ConnectRegistersAffinity) {
    sender s;
    auto r = std::make_shared<receiver>();
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

    auto c = utils::connect(s.value_changed, r, &receiver::on_value, &worker,
                            utils::connection_type::queued);
    EXPECT_TRUE(c.connected());
    EXPECT_EQ(utils::find_slot_affinity(r.get(), &receiver::on_value), &worker);
    s.value_changed.emit(3);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 3; }));
    worker.stop();
}

TEST(Signal, DifferentSlotsDifferentThreads) {
    utils::thread w1;
    utils::thread w2;
    w1.start();
    w2.start();
    ASSERT_TRUE(wait_until([&] { return w1.is_running() && w2.is_running(); }));

    struct multi {
        std::atomic<int> a{0};
        std::atomic<int> b{0};
        utils::slots_t<> slot_a(int v) {
            a = v;
            return {};
        }
        utils::slots_t<> slot_b(int v) {
            b = v;
            return {};
        }
    };

    utils::signal<int> sig;
    auto m = std::make_shared<multi>();
    (void)utils::connect(sig, m, &multi::slot_a, &w1,
                         utils::connection_type::queued);
    (void)utils::connect(sig, m, &multi::slot_b, &w2,
                         utils::connection_type::queued);
    EXPECT_EQ(utils::find_slot_affinity(m.get(), &multi::slot_a), &w1);
    EXPECT_EQ(utils::find_slot_affinity(m.get(), &multi::slot_b), &w2);

    sig.emit(11);
    ASSERT_TRUE(wait_until(
        [&] { return m->a.load() == 11 && m->b.load() == 11; }));
    w1.stop();
    w2.stop();
}

// =============================================================================
// invoke
// =============================================================================

TEST(Invoke, DirectViaSptr) {
    auto r = std::make_shared<receiver>();
    auto result = utils::invoke(r, &receiver::twice, 21);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 42);
}

TEST(Invoke, ExplicitThreadBlocking) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    auto r = std::make_shared<receiver>();
    auto result = utils::invoke(r, &receiver::twice, &worker,
                                utils::connection_type::blocking_queued, 5);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 10);
    worker.stop();
}

TEST(Invoke, LookupAffinityTable) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    auto r = std::make_shared<receiver>();
    utils::bind_slot_affinity(r.get(), &receiver::twice, &worker);
    auto result = utils::invoke(r, &receiver::twice,
                                utils::connection_type::blocking_queued, 6);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 12);
    worker.stop();
}

TEST(Invoke, CallableRequiresExplicitThread) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    auto result = utils::invoke(&worker, utils::connection_type::blocking_queued,
                                [](int x) { return x + 1; }, 40);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 41);

    std::atomic<int> value{0};
    utils::invoke(&worker, [&] { value = 55; }, utils::connection_type::queued);
    ASSERT_TRUE(wait_until([&] { return value.load() == 55; }));
    worker.stop();
}

TEST(Invoke, WptrReceiver) {
    auto r = std::make_shared<receiver>();
    utils::wptr<receiver> wp = r;
    auto result = utils::invoke(wp, &receiver::twice_const, 3);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 6);
}

// =============================================================================
// 补充：公开 API 缺口覆盖
// =============================================================================

TEST(SlotsT, MoveGetAndAssign) {
    utils::slots_t<int> a(7);
    utils::slots_t<int> b;
    b = a;
    EXPECT_EQ(b.get(), 7);
    const utils::slots_t<int> c(9);
    EXPECT_EQ(c.get(), 9);
    utils::slots_t<int> d(11);
    EXPECT_EQ(std::move(d).get(), 11);
}

TEST(Connection, IdAndDoubleDisconnect) {
    utils::connection empty;
    EXPECT_EQ(empty.id(), 0u);

    sender s;
    auto r = std::make_shared<receiver>();
    auto conn = utils::connect(s.value_changed, r, &receiver::on_value,
                               utils::connection_type::direct);
    EXPECT_NE(conn.id(), 0u);
    EXPECT_TRUE(conn.connected());
    conn.disconnect();
    EXPECT_FALSE(conn.connected());
    EXPECT_EQ(conn.id(), 0u);
    EXPECT_NO_THROW(conn.disconnect());
}

TEST(ScopedConnection, ReleaseKeepsConnection) {
    sender s;
    auto r = std::make_shared<receiver>();
    utils::connection kept;
    {
        utils::scoped_connection scoped(utils::connect(
            s.value_changed, r, &receiver::on_value,
            utils::connection_type::direct));
        EXPECT_TRUE(scoped.connected());
        kept = scoped.release();
        EXPECT_FALSE(scoped.connected());
    }
    EXPECT_TRUE(kept.connected());
    s.value_changed.emit(3);
    EXPECT_EQ(r->last.load(), 3);
    kept.disconnect();
}

TEST(ScopedConnection, MoveAssignDisconnectsOld) {
    sender s;
    auto r1 = std::make_shared<receiver>();
    auto r2 = std::make_shared<receiver>();
    utils::scoped_connection a(utils::connect(s.value_changed, r1,
                                              &receiver::on_value,
                                              utils::connection_type::direct));
    utils::scoped_connection b(utils::connect(s.value_changed, r2,
                                              &receiver::on_value,
                                              utils::connection_type::direct));
    a = std::move(b);
    EXPECT_TRUE(a.connected());
    s.value_changed.emit(1);
    EXPECT_EQ(r1->calls, 0);
    EXPECT_EQ(r2->last.load(), 1);
}

TEST(ScopedConnection, ExplicitDisconnect) {
    sender s;
    auto r = std::make_shared<receiver>();
    utils::scoped_connection scoped(utils::connect(
        s.value_changed, r, &receiver::on_value,
        utils::connection_type::direct));
    scoped.disconnect();
    EXPECT_FALSE(scoped.connected());
    s.value_changed.emit(1);
    EXPECT_EQ(r->calls, 0);
}

TEST(Signal, InitiallyBlockedAndSignalsBlocked) {
    utils::signal<int> sig(true);
    EXPECT_TRUE(sig.signals_blocked());
    auto r = std::make_shared<receiver>();
    (void)utils::connect(sig, r, &receiver::on_value,
                         utils::connection_type::direct);
    sig.emit(1);
    EXPECT_EQ(r->calls, 0);
    sig.block_signals(false);
    EXPECT_FALSE(sig.signals_blocked());
    sig.emit(2);
    EXPECT_EQ(r->last.load(), 2);
}

TEST(Signal, DisconnectViaConnectionHandle) {
    sender s;
    auto r = std::make_shared<receiver>();
    auto conn = utils::connect(s.value_changed, r, &receiver::on_value,
                               utils::connection_type::direct);
    s.value_changed.disconnect(conn);
    EXPECT_FALSE(conn.connected());
    s.value_changed.emit(1);
    EXPECT_EQ(r->calls, 0);
}

TEST(Signal, DisconnectByWptr) {
    sender s;
    auto r = std::make_shared<receiver>();
    utils::wptr<receiver> wp = r;
    (void)utils::connect(s.value_changed, r, &receiver::on_value,
                         utils::connection_type::direct);
    s.value_changed.disconnect(wp);
    s.value_changed.emit(1);
    EXPECT_EQ(r->calls, 0);
}

TEST(Signal, DestructorDisconnectsSlots) {
    auto r = std::make_shared<receiver>();
    utils::connection conn;
    {
        utils::signal<int> sig;
        conn = utils::connect(sig, r, &receiver::on_value,
                              utils::connection_type::direct);
        EXPECT_TRUE(conn.connected());
        sig.emit(1);
        EXPECT_EQ(r->last.load(), 1);
    }
    EXPECT_FALSE(conn.connected());
}

TEST(Signal, UniqueAllowsDifferentMethodsAndReconnect) {
    sender s;
    auto r = std::make_shared<receiver>();
    auto c1 = utils::connect(s.value_changed, r, &receiver::on_value,
                             utils::connection_type::direct,
                             utils::unique_connection);
    auto c2 = utils::connect(s.value_changed, r, &receiver::on_value_const,
                             utils::connection_type::direct,
                             utils::unique_connection);
    EXPECT_TRUE(c1.connected());
    EXPECT_TRUE(c2.connected());
    c1.disconnect();
    auto c3 = utils::connect(s.value_changed, r, &receiver::on_value,
                             utils::connection_type::direct,
                             utils::unique_connection);
    EXPECT_TRUE(c3.connected());
}

TEST(Signal, UniqueIgnoredForLambda) {
    sender s;
    auto r = std::make_shared<receiver>();
    int a = 0;
    int b = 0;
    auto c1 = utils::connect(
        s.value_changed, r, [&](int v) { a = v; },
        utils::connection_type::direct, utils::unique_connection);
    auto c2 = utils::connect(
        s.value_changed, r, [&](int v) { b = v; },
        utils::connection_type::direct, utils::unique_connection);
    EXPECT_TRUE(c1.connected());
    EXPECT_TRUE(c2.connected());
    s.value_changed.emit(4);
    EXPECT_EQ(a, 4);
    EXPECT_EQ(b, 4);
}

TEST(Signal, AutomaticSameThreadDirect) {
    sender s;
    auto r = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r, &receiver::on_value,
                         utils::connection_type::automatic);
    s.value_changed.emit(12);
    EXPECT_EQ(r->last.load(), 12);
}

TEST(Signal, AutomaticCrossThreadQueues) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    sender s;
    auto r = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r, &receiver::on_value, &worker,
                         utils::connection_type::automatic);
    s.value_changed.emit(15);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 15; }));
    worker.stop();
}

TEST(Signal, ConnectUsesAffinityWithoutExplicitThread) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    sender s;
    auto r = std::make_shared<receiver>();
    utils::bind_slot_affinity(r, &receiver::on_value, &worker);
    (void)utils::connect(s.value_changed, r, &receiver::on_value,
                         utils::connection_type::queued);
    s.value_changed.emit(21);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 21; }));
    worker.stop();
}

TEST(Signal, QueuedWithoutTargetIsSkipped) {
    sender s;
    auto r = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r, &receiver::on_value,
                         utils::connection_type::queued);
    s.value_changed.emit(1);
    EXPECT_EQ(r->calls, 0);
}

TEST(Signal, BlockingQueuedSameThreadRunsDirect) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    sender s;
    auto r = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r, &receiver::on_value, &worker,
                         utils::connection_type::blocking_queued);
    EXPECT_TRUE(worker.loop()->post_blocking([&] { s.value_changed.emit(33); }));
    EXPECT_EQ(r->last.load(), 33);
    worker.stop();
}

TEST(Signal, ConstMemberViaConnect) {
    sender s;
    auto r = std::make_shared<receiver>();
    (void)utils::connect(s.value_changed, r, &receiver::on_value_const,
                         utils::connection_type::direct);
    s.value_changed.emit(8);
    EXPECT_EQ(r->last.load(), 8);
}

TEST(Connect, WptrWithExplicitThread) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    sender s;
    auto r = std::make_shared<receiver>();
    utils::wptr<receiver> wp = r;
    (void)utils::connect(s.value_changed, wp, &receiver::on_value, &worker,
                         utils::connection_type::queued);
    s.value_changed.emit(44);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 44; }));
    worker.stop();
}

TEST(Connect, LambdaWithExplicitThread) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    sender s;
    auto owner = std::make_shared<receiver>();
    std::atomic<int> captured{0};
    (void)utils::connect(
        s.value_changed, owner,
        [&](int v) { captured = v; }, &worker, utils::connection_type::queued);
    s.value_changed.emit(55);
    ASSERT_TRUE(wait_until([&] { return captured.load() == 55; }));
    worker.stop();
}

TEST(Connect, WptrLambda) {
    sender s;
    auto r = std::make_shared<receiver>();
    utils::wptr<receiver> wp = r;
    int captured = 0;
    (void)utils::connect(
        s.value_changed, wp, [&](int v) { captured = v; },
        utils::connection_type::direct);
    s.value_changed.emit(6);
    EXPECT_EQ(captured, 6);
}

TEST(Connect, NullOrExpiredReturnsEmpty) {
    sender s;
    utils::sptr<receiver> null_r;
    auto c1 = utils::connect(s.value_changed, null_r, &receiver::on_value);
    EXPECT_FALSE(c1.connected());

    utils::wptr<receiver> dead;
    auto c2 = utils::connect(s.value_changed, dead, &receiver::on_value);
    EXPECT_FALSE(c2.connected());
}

TEST(Invoke, QueuedReturnsInProgress) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    auto r = std::make_shared<receiver>();
    auto result = utils::invoke(r, &receiver::twice, &worker,
                                utils::connection_type::queued, 2);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(),
              std::make_error_code(std::errc::operation_in_progress));
    worker.stop();
}

TEST(Invoke, AutomaticCrossThreadInProgress) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    auto r = std::make_shared<receiver>();
    utils::bind_slot_affinity(r, &receiver::twice, &worker);
    auto result = utils::invoke(r, &receiver::twice,
                                utils::connection_type::automatic, 2);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(),
              std::make_error_code(std::errc::operation_in_progress));
    worker.stop();
}

TEST(Invoke, VoidSlotReturnsOk) {
    auto r = std::make_shared<receiver>();
    auto result = utils::invoke(r, &receiver::on_value, 3);
    ASSERT_TRUE(result);
    EXPECT_EQ(r->last.load(), 3);
}

TEST(Invoke, ConstPmfViaSptr) {
    auto r = std::make_shared<receiver>();
    auto result = utils::invoke(r, &receiver::twice_const, 4);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 8);
}

TEST(Invoke, WptrWithThreadBlocking) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    auto r = std::make_shared<receiver>();
    utils::wptr<receiver> wp = r;
    auto result = utils::invoke(wp, &receiver::twice, &worker,
                                utils::connection_type::blocking_queued, 6);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 12);
    worker.stop();
}

TEST(Invoke, ExpiredWptrOwnerDead) {
    utils::wptr<receiver> dead;
    auto result = utils::invoke(dead, &receiver::twice, 1);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::make_error_code(std::errc::owner_dead));
}

TEST(Invoke, NullReceiverInvalidArgument) {
    utils::sptr<receiver> null_r;
    auto result = utils::invoke(null_r, &receiver::twice, 1);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(),
              std::make_error_code(std::errc::invalid_argument));
}

TEST(Invoke, BlockingFailsWhenNotPumping) {
    utils::thread worker;
    // not started → loop null / not pumping
    auto r = std::make_shared<receiver>();
    auto result = utils::invoke(r, &receiver::twice, &worker,
                                utils::connection_type::blocking_queued, 1);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(),
              std::make_error_code(std::errc::operation_not_permitted));
}

TEST(Invoke, CallableDirectReturnsValue) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    auto result = utils::invoke(
        &worker, utils::connection_type::direct, [](int x) { return x + 1; },
        40);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 41);
    worker.stop();
}

TEST(Invoke, FireAndForgetDirectAndBlocking) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));
    std::atomic<int> value{0};
    utils::invoke(&worker, [&] { value = 1; }, utils::connection_type::direct);
    EXPECT_EQ(value.load(), 1);
    utils::invoke(&worker, [&] { value = 2; },
                  utils::connection_type::blocking_queued);
    EXPECT_EQ(value.load(), 2);
    utils::invoke(static_cast<utils::thread *>(nullptr),
                  std::function<void()>{[] {}},
                  utils::connection_type::direct); // null target noop for non-direct paths; direct still runs if fn set — use empty fn
    utils::invoke(&worker, std::function<void()>{},
                  utils::connection_type::queued); // empty fn noop
    worker.stop();
}

// =============================================================================
// 压力 / 并发
// =============================================================================

TEST(Stress, ManyDirectEmitsSingleSlot) {
    constexpr int N = 200000;
    sender s;
    auto r = std::make_shared<receiver>();
    utils::scoped_connection c{utils::connect(
        s.value_changed, r, &receiver::on_value,
        utils::connection_type::direct)};
    for (int i = 0; i < N; ++i) {
        s.value_changed.emit(i);
    }
    EXPECT_EQ(r->calls, N);
    EXPECT_EQ(r->last.load(), N - 1);
}

TEST(Stress, FanOutManyReceivers) {
    constexpr int receivers_n = 64;
    constexpr int emits_n = 5000;
    sender s;
    std::vector<std::shared_ptr<receiver>> rs;
    std::vector<utils::scoped_connection> cons;
    rs.reserve(receivers_n);
    cons.reserve(receivers_n);
    for (int i = 0; i < receivers_n; ++i) {
        rs.push_back(std::make_shared<receiver>());
        cons.emplace_back(utils::connect(s.value_changed, rs.back(),
                                         &receiver::on_value,
                                         utils::connection_type::direct));
    }
    for (int i = 0; i < emits_n; ++i) {
        s.value_changed.emit(i);
    }
    for (auto &r : rs) {
        EXPECT_EQ(r->calls, emits_n);
        EXPECT_EQ(r->last.load(), emits_n - 1);
    }
}

TEST(Stress, MultiThreadEmitSameSignal) {
    constexpr int threads_n = 8;
    constexpr int emits_per_thread = 20000;
    sender s;
    auto r = std::make_shared<receiver>();
    // 用原子累加槽，避免多线程写 calls/last 数据竞争
    std::atomic<int> hits{0};
    utils::scoped_connection c{utils::connect(
        s.value_changed, r,
        [&](int) { hits.fetch_add(1, std::memory_order_relaxed); },
        utils::connection_type::direct)};

    std::vector<std::thread> pool;
    pool.reserve(threads_n);
    for (int t = 0; t < threads_n; ++t) {
        pool.emplace_back([&] {
            for (int i = 0; i < emits_per_thread; ++i) {
                s.value_changed.emit(i);
            }
        });
    }
    for (auto &th : pool) {
        th.join();
    }
    EXPECT_EQ(hits.load(), threads_n * emits_per_thread);
}

TEST(Stress, EmitWhileConnectAndDisconnect) {
    constexpr int emitters_n = 4;
    constexpr int mutators_n = 2;
    constexpr int emits_per = 10000;
    constexpr int mutate_rounds = 2000;

    sender s;
    std::atomic<bool> stop{false};
    std::atomic<int> hits{0};
    auto keep_alive = std::make_shared<receiver>();

    std::vector<std::thread> pool;
    for (int t = 0; t < emitters_n; ++t) {
        pool.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                s.value_changed.emit(1);
            }
            for (int i = 0; i < emits_per; ++i) {
                s.value_changed.emit(1);
            }
        });
    }
    for (int t = 0; t < mutators_n; ++t) {
        pool.emplace_back([&] {
            for (int i = 0; i < mutate_rounds; ++i) {
                auto r = std::make_shared<receiver>();
                auto conn = utils::connect(
                    s.value_changed, r,
                    [&](int) { hits.fetch_add(1, std::memory_order_relaxed); },
                    utils::connection_type::direct);
                s.value_changed.emit(1);
                conn.disconnect();
            }
        });
    }

    // 常驻连接，确保 emit 路径始终有活槽
    utils::scoped_connection sticky{utils::connect(
        s.value_changed, keep_alive,
        [&](int) { hits.fetch_add(1, std::memory_order_relaxed); },
        utils::connection_type::direct)};

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_release);
    for (auto &th : pool) {
        th.join();
    }
    EXPECT_GT(hits.load(), 0);
}

TEST(Stress, QueuedFloodToWorker) {
    constexpr int N = 20000;
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));

    sender s;
    auto r = std::make_shared<receiver>();
    std::atomic<int> hits{0};
    utils::scoped_connection c{utils::connect(
        s.value_changed, r,
        [&](int) { hits.fetch_add(1, std::memory_order_relaxed); }, &worker,
        utils::connection_type::queued)};

    for (int i = 0; i < N; ++i) {
        s.value_changed.emit(i);
    }
    ASSERT_TRUE(wait_until(
        [&] { return hits.load() == N; },
        std::chrono::milliseconds(5000)));
    worker.stop();
}

TEST(Stress, MultiProducerQueuedFlood) {
    constexpr int producers = 4;
    constexpr int per_producer = 5000;
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        return worker.loop() && worker.loop()->is_pumping();
    }));

    sender s;
    auto r = std::make_shared<receiver>();
    std::atomic<int> hits{0};
    utils::scoped_connection c{utils::connect(
        s.value_changed, r,
        [&](int) { hits.fetch_add(1, std::memory_order_relaxed); }, &worker,
        utils::connection_type::queued)};

    std::vector<std::thread> pool;
    for (int p = 0; p < producers; ++p) {
        pool.emplace_back([&] {
            for (int i = 0; i < per_producer; ++i) {
                s.value_changed.emit(i);
            }
        });
    }
    for (auto &th : pool) {
        th.join();
    }
    ASSERT_TRUE(wait_until(
        [&] { return hits.load() == producers * per_producer; },
        std::chrono::milliseconds(5000)));
    worker.stop();
}

#include "concurrency/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

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

struct sender : utils::object {
    utils::signal<int> value_changed{this};
    ~sender() override { invalidate(); }
};

struct receiver : utils::object {
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

    ~receiver() override { invalidate(); }
};

} // namespace

// =============================================================================
// slots_t
// =============================================================================

TEST(SlotsT, ValueConstructConvertAndGet) {
    utils::slots_t<int> s(42);
    EXPECT_EQ(static_cast<int>(s), 42);
    EXPECT_EQ(s.get(), 42);

    const utils::slots_t<int> cs(7);
    EXPECT_EQ(cs.get(), 7);

    utils::slots_t<int> moved(99);
    EXPECT_EQ(std::move(moved).get(), 99);
}

TEST(SlotsT, CopyAssignAndDefault) {
    utils::slots_t<int> a(3);
    utils::slots_t<int> b(a);
    EXPECT_EQ(b.get(), 3);
    utils::slots_t<int> c;
    c = a;
    EXPECT_EQ(c.get(), 3);
    utils::slots_t<int> d(std::move(a));
    EXPECT_EQ(d.get(), 3);
}

TEST(SlotsT, VoidSpecialization) {
    utils::slots_t<> v;
    utils::slots_t<> copy = v;
    (void)copy;
}

// =============================================================================
// event_loop
// =============================================================================

TEST(EventLoop, PostAndProcessEvents) {
    utils::event_loop loop;
    int value = 0;
    EXPECT_TRUE(loop.post([&] { value = 1; }));
    EXPECT_TRUE(loop.is_running());
    loop.process_events();
    EXPECT_EQ(value, 1);
}

TEST(EventLoop, PostEmptyTaskFails) {
    utils::event_loop loop;
    EXPECT_FALSE(loop.post({}));
}

TEST(EventLoop, PostAfterStopFailsAndSetAcceptingRestores) {
    utils::event_loop loop;
    loop.stop();
    EXPECT_FALSE(loop.is_running());
    EXPECT_FALSE(loop.post([] {}));

    loop.set_accepting(true);
    EXPECT_TRUE(loop.is_running());
    int value = 0;
    EXPECT_TRUE(loop.post([&] { value = 5; }));
    loop.process_events();
    EXPECT_EQ(value, 5);
}

TEST(EventLoop, PostDelayedZeroActsAsPost) {
    utils::event_loop loop;
    int value = 0;
    EXPECT_EQ(loop.post_delayed(std::chrono::milliseconds(0), [&] { value = 1; }),
              0u);
    loop.process_events();
    EXPECT_EQ(value, 1);
}

TEST(EventLoop, PostDelayedHonorsBudgetAndCancel) {
    utils::event_loop loop;
    int value = 0;
    (void)loop.post_delayed(std::chrono::milliseconds(20), [&] { value = 1; });
    loop.process_events();
    EXPECT_EQ(value, 0);

    loop.process_events(std::chrono::milliseconds(200));
    EXPECT_EQ(value, 1);

    auto id =
        loop.post_delayed(std::chrono::milliseconds(20), [&] { value = 2; });
    loop.cancel_timer(id);
    loop.process_events(std::chrono::milliseconds(80));
    EXPECT_EQ(value, 1);
}

TEST(EventLoop, PostPeriodicAndCancel) {
    utils::event_loop loop;
    EXPECT_EQ(loop.post_periodic(std::chrono::milliseconds(0), [] {}), 0u);
    EXPECT_EQ(loop.post_periodic(std::chrono::milliseconds(10), {}), 0u);

    int ticks = 0;
    auto id = loop.post_periodic(std::chrono::milliseconds(15), [&] { ++ticks; });
    ASSERT_NE(id, 0u);
    loop.process_events(std::chrono::milliseconds(80));
    EXPECT_GE(ticks, 2);
    loop.cancel_timer(id);
    const int after = ticks;
    loop.process_events(std::chrono::milliseconds(50));
    EXPECT_EQ(ticks, after);
}

TEST(EventLoop, CancelTimerIdZeroIsNoop) {
    utils::event_loop loop;
    loop.cancel_timer(0);
}

TEST(EventLoop, IsPumpingFlagsDuringProcessEvents) {
    utils::event_loop loop;
    EXPECT_FALSE(loop.is_pumping());
    EXPECT_FALSE(loop.is_pumping_on_current_thread());

    bool saw_pumping = false;
    bool saw_on_current = false;
    loop.post([&] {
        saw_pumping = loop.is_pumping();
        saw_on_current = loop.is_pumping_on_current_thread();
    });
    loop.process_events();
    EXPECT_TRUE(saw_pumping);
    EXPECT_TRUE(saw_on_current);
    EXPECT_FALSE(loop.is_pumping());
}

TEST(EventLoop, RunStopsFromOtherThread) {
    utils::event_loop loop;
    std::atomic<int> value{0};
    EXPECT_TRUE(loop.post([&] { value = 7; }));

    std::thread stopper([&] {
        ASSERT_TRUE(wait_until([&] { return loop.is_pumping(); }));
        loop.stop();
    });
    loop.run();
    stopper.join();
    EXPECT_EQ(value.load(), 7);
    EXPECT_FALSE(loop.is_running());
}

TEST(EventLoop, PostBlockingFromOtherThreadToPumpingLoopSucceeds) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_running() && loop->is_pumping();
    }));

    utils::event_loop *loop = worker.loop();
    ASSERT_NE(loop, nullptr);
    EXPECT_TRUE(loop->is_pumping());
    EXPECT_FALSE(loop->is_pumping_on_current_thread());

    std::atomic<int> value{0};
    const bool ok = loop->post_blocking([&] { value = 42; });
    EXPECT_TRUE(ok);
    EXPECT_EQ(value.load(), 42);
    worker.stop();
}

TEST(EventLoop, PostBlockingFromWithinSamePumpingLoopFails) {
    utils::event_loop loop;
    std::atomic<bool> nested_ok{true};
    std::atomic<int> outer{0};

    loop.post([&] {
        outer = 1;
        nested_ok = loop.post_blocking([&] { outer = 2; });
    });
    loop.process_events();

    EXPECT_EQ(outer.load(), 1);
    EXPECT_FALSE(nested_ok.load());
}

TEST(EventLoop, PostBlockingToIdleLoopFailsWithoutHanging) {
    utils::event_loop *loop = utils::ensure_thread()->loop();
    ASSERT_NE(loop, nullptr);
    EXPECT_FALSE(loop->is_pumping());

    const auto started = std::chrono::steady_clock::now();
    const bool ok = loop->post_blocking([] {});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_FALSE(ok);
    EXPECT_LT(elapsed, std::chrono::milliseconds(200));
}

TEST(EventLoop, PostBlockingEmptyTaskFails) {
    utils::event_loop loop;
    EXPECT_FALSE(loop.post_blocking({}));
}

TEST(EventLoop, QueuedTaskExceptionDoesNotKillLoop) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    utils::event_loop *loop = worker.loop();
    std::atomic<int> value{0};
    loop->post([] { throw std::runtime_error("slot boom"); });
    loop->post([&] { value = 9; });
    ASSERT_TRUE(wait_until([&] { return value.load() == 9; }));
    worker.stop();
}

TEST(EventLoop, RunWakesForEarlierTimer) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    utils::event_loop *loop = worker.loop();
    ASSERT_NE(loop, nullptr);

    std::atomic<int> which{0};
    (void)loop->post_delayed(std::chrono::milliseconds(400), [&] { which = 2; });
    ASSERT_TRUE(wait_until([&] { return loop->is_pumping(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    (void)loop->post_delayed(std::chrono::milliseconds(20), [&] { which = 1; });

    ASSERT_TRUE(wait_until([&] { return which.load() == 1; },
                           std::chrono::milliseconds(250)));
    worker.stop();
    EXPECT_EQ(which.load(), 1);
}

TEST(EventLoop, NestedProcessEventsDoesNotClearOuterPumping) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    utils::event_loop *loop = worker.loop();
    std::atomic<bool> nested_ran{false};
    std::atomic<bool> still_pumping{false};

    loop->post([&] {
        loop->post([&] { nested_ran = true; });
        loop->process_events();
        still_pumping = loop->is_pumping();
    });

    ASSERT_TRUE(wait_until([&] { return nested_ran.load(); }));
    EXPECT_TRUE(still_pumping.load());
    worker.stop();
}

TEST(EventLoop, AbandonedBlockingPostDoesNotRunLater) {
    utils::event_loop loop;
    std::atomic<int> value{0};

    loop.post([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    });

    std::atomic<bool> waiter_done{false};
    std::thread waiter([&] {
        ASSERT_TRUE(wait_until([&] { return loop.is_pumping(); }));
        const bool ok = loop.post_blocking([&] { value = 1; });
        EXPECT_FALSE(ok);
        waiter_done = true;
    });

    loop.process_events(std::chrono::milliseconds(15));
    waiter.join();
    EXPECT_TRUE(waiter_done.load());

    loop.process_events();
    EXPECT_EQ(value.load(), 0);
}

TEST(EventLoop, PeriodicTimerReschedulesAfterTaskException) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    std::atomic<int> ticks{0};
    auto id = worker.loop()->post_periodic(std::chrono::milliseconds(20), [&] {
        const int n = ticks.fetch_add(1) + 1;
        if (n == 1) {
            throw std::runtime_error("timer boom");
        }
    });
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(wait_until([&] { return ticks.load() >= 3; }));
    worker.loop()->cancel_timer(id);
    worker.stop();
}

TEST(EventLoop, ProcessEventsDoesNotLeaveDanglingThreadDefault) {
    {
        utils::event_loop loop;
        loop.process_events();
    }
    sender s;
    receiver r;
    s.value_changed.connect(&r, &receiver::on_value,
                            utils::connection_type::direct);
    s.value_changed.emit(11);
    EXPECT_EQ(r.last, 11);
}

// =============================================================================
// thread / current_thread / ensure_thread
// =============================================================================

TEST(Thread, EnsureThreadReturnsStableHandleAndLoop) {
    utils::thread *t1 = utils::ensure_thread();
    utils::thread *t2 = utils::ensure_thread();
    ASSERT_NE(t1, nullptr);
    EXPECT_EQ(t1, t2);
    EXPECT_NE(t1->loop(), nullptr);
    EXPECT_EQ(t1->loop_shared().get(), t1->loop());
    EXPECT_FALSE(t1->identity().expired());
    EXPECT_TRUE(t1->is_running());
}

TEST(Thread, CurrentThreadStartStopPumpsOnCallerThread) {
    utils::thread *t = utils::ensure_thread();
    ASSERT_NE(t, nullptr);
    ASSERT_NE(t->loop(), nullptr);

    std::atomic<int> value{0};
    t->loop()->post([&] { value = 7; });

    std::thread stopper([t] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        t->stop();
    });
    t->start();
    stopper.join();

    EXPECT_EQ(value.load(), 7);
    EXPECT_TRUE(t->is_running()); // start 返回后 set_accepting(true)
}

// =============================================================================
// core_application
// =============================================================================

TEST(CoreApplication, ThreadLoopAndExec) {
    utils::core_application app;
    EXPECT_EQ(app.thread(), utils::ensure_thread());
    EXPECT_EQ(app.loop(), utils::ensure_thread()->loop());

    std::atomic<int> value{0};
    app.loop()->post([&] { value = 3; });

    std::thread stopper([&] {
        ASSERT_TRUE(wait_until([&] { return app.loop()->is_pumping(); }));
        app.thread()->stop();
    });
    EXPECT_EQ(app.exec(), 0);
    stopper.join();
    EXPECT_EQ(value.load(), 3);
}

// =============================================================================
// worker_thread
// =============================================================================

TEST(WorkerThread, StartStopLoopAndIsRunning) {
    utils::worker_thread worker;
    EXPECT_FALSE(worker.is_running());
    EXPECT_EQ(worker.loop(), nullptr);
    EXPECT_FALSE(worker.identity().expired());

    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    EXPECT_NE(worker.loop(), nullptr);
    EXPECT_EQ(worker.loop_shared().get(), worker.loop());

    worker.start(); // 幂等
    EXPECT_TRUE(worker.is_running());

    worker.stop();
    EXPECT_FALSE(worker.is_running());
    EXPECT_EQ(worker.loop(), nullptr);
}

TEST(WorkerThread, StartThenImmediateStopDoesNotHang) {
    for (int i = 0; i < 40; ++i) {
        utils::worker_thread worker;
        worker.start();
        worker.stop();
        EXPECT_EQ(worker.loop(), nullptr);
    }
}

// =============================================================================
// connection
// =============================================================================

TEST(Connection, DefaultDisconnected) {
    utils::connection c;
    EXPECT_FALSE(c.connected());
    EXPECT_EQ(c.id(), 0u);
    c.disconnect();
}

TEST(Connection, ConnectedDisconnectAndId) {
    sender s;
    receiver r;
    auto conn = s.value_changed.connect(&r, &receiver::on_value,
                                        utils::connection_type::direct);
    EXPECT_TRUE(conn.connected());
    EXPECT_NE(conn.id(), 0u);
    const auto id = conn.id();
    (void)id;
    conn.disconnect();
    EXPECT_FALSE(conn.connected());
    EXPECT_EQ(conn.id(), 0u);
}

// =============================================================================
// scoped_connection
// =============================================================================

TEST(ScopedConnection, RaIIDisconnects) {
    sender s;
    receiver r;
    {
        utils::scoped_connection scoped(s.value_changed.connect(
            &r, &receiver::on_value, utils::connection_type::direct));
        EXPECT_TRUE(scoped.connected());
        s.value_changed.emit(4);
        EXPECT_EQ(r.last, 4);
    }
    s.value_changed.emit(5);
    EXPECT_EQ(r.last, 4);
}

TEST(ScopedConnection, MoveAssignAndRelease) {
    sender s;
    receiver r;
    utils::scoped_connection a(s.value_changed.connect(
        &r, &receiver::on_value, utils::connection_type::direct));
    utils::scoped_connection b(std::move(a));
    EXPECT_FALSE(a.connected());
    EXPECT_TRUE(b.connected());

    utils::scoped_connection c;
    c = std::move(b);
    EXPECT_FALSE(b.connected());
    EXPECT_TRUE(c.connected());

    auto released = c.release();
    EXPECT_FALSE(c.connected());
    EXPECT_TRUE(released.connected());
    released.disconnect();
}

TEST(ScopedConnection, ExplicitDisconnect) {
    sender s;
    receiver r;
    utils::scoped_connection scoped(s.value_changed.connect(
        &r, &receiver::on_value, utils::connection_type::direct));
    scoped.disconnect();
    EXPECT_FALSE(scoped.connected());
    s.value_changed.emit(1);
    EXPECT_EQ(r.calls, 0);
}

// =============================================================================
// object (+ helpers)
// =============================================================================

TEST(Object, LifetimeIsValidAndInvalidate) {
    receiver r;
    EXPECT_TRUE(r.is_valid());
    auto weak = r.lifetime();
    EXPECT_FALSE(weak.expired());
    r.invalidate();
    EXPECT_FALSE(r.is_valid());
    EXPECT_TRUE(weak.expired());
    r.invalidate(); // 幂等
}

TEST(Object, MoveToThreadWorkerAndBack) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

    receiver r;
    EXPECT_EQ(r.thread(), utils::ensure_thread());
    EXPECT_EQ(r.worker(), nullptr);
    EXPECT_EQ(r.loop(), utils::ensure_thread()->loop());
    EXPECT_EQ(r.loop_shared().get(), r.loop());

    r.move_to_thread(worker);
    EXPECT_EQ(r.thread(), &worker);
    EXPECT_EQ(r.worker(), &worker);
    EXPECT_NE(r.loop(), nullptr);

    r.move_to_thread(nullptr);
    EXPECT_EQ(r.thread(), utils::ensure_thread());
    EXPECT_EQ(r.worker(), nullptr);
    worker.stop();
}

TEST(Object, BlockSignals) {
    sender s;
    EXPECT_FALSE(s.signals_blocked());
    s.block_signals(true);
    EXPECT_TRUE(s.signals_blocked());
    s.block_signals(false);
    EXPECT_FALSE(s.signals_blocked());
}

TEST(Object, TrackAndUntrackInbound) {
    receiver r;
    auto state = std::make_shared<utils::connection_state>();
    state->_id = 42;
    EXPECT_TRUE(r.track_inbound(state));
    r.untrack_inbound(42);

    r.invalidate();
    auto dead = std::make_shared<utils::connection_state>();
    dead->_id = 1;
    EXPECT_FALSE(r.track_inbound(dead));
}

TEST(Object, DeleteLaterOnCurrentThread) {
    auto *raw = new receiver();
    raw->delete_later();
}

TEST(Object, DeleteLaterAfterWorkerStopDoesNotLeak) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

    auto obj = utils::make_object_unique<receiver>();
    obj->move_to_thread(worker);
    worker.stop();
    EXPECT_EQ(obj->loop(), nullptr);
    obj.reset();
}

TEST(Object, MakeObjectUniqueSharedAndObjectGet) {
    auto up = utils::make_object_unique<receiver>();
    ASSERT_NE(up, nullptr);
    EXPECT_EQ(utils::object_get(up), up.get());

    receiver stack;
    EXPECT_EQ(utils::object_get(&stack), &stack);

    auto sp = utils::make_object_shared<receiver>();
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(utils::object_get(sp), sp.get());

    utils::object_wptr<receiver> wp = sp;
    EXPECT_FALSE(wp.expired());
    sp.reset();
}

TEST(Object, ReceiverDestructorDropsInboundConnection) {
    sender s;
    {
        receiver r;
        s.value_changed.connect(&r, &receiver::on_value,
                                utils::connection_type::direct);
        s.value_changed.emit(1);
        EXPECT_EQ(r.last, 1);
    }
    s.value_changed.emit(2);
}

TEST(Object, WorkerAffinitySurvivesStopAndRestart) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

    sender s;
    auto r = std::make_unique<receiver>();
    r->move_to_thread(worker);
    s.value_changed.connect(r.get(), &receiver::on_value,
                            utils::connection_type::queued);

    s.value_changed.emit(1);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 1; }));

    worker.stop();
    EXPECT_EQ(r->thread(), &worker);
    EXPECT_EQ(r->loop(), nullptr);
    s.value_changed.emit(2);
    EXPECT_EQ(r->last.load(), 1);

    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    ASSERT_NE(r->loop(), nullptr);
    s.value_changed.emit(3);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 3; }));
    worker.stop();

    r->move_to_thread(nullptr);
    EXPECT_EQ(r->thread(), utils::ensure_thread());
    EXPECT_EQ(r->worker(), nullptr);
}

// =============================================================================
// signal
// =============================================================================

TEST(Signal, DirectEmitAndOperatorCall) {
    sender s;
    receiver r;
    auto conn = s.value_changed.connect(&r, &receiver::on_value,
                                        utils::connection_type::direct);
    EXPECT_TRUE(conn.connected());
    s.value_changed.emit(5);
    EXPECT_EQ(r.last, 5);
    EXPECT_EQ(r.calls, 1);

    s.value_changed(9);
    EXPECT_EQ(r.last, 9);
    EXPECT_EQ(r.calls, 2);

    conn.disconnect();
    EXPECT_FALSE(conn.connected());
    s.value_changed.emit(6);
    EXPECT_EQ(r.last, 9);
    EXPECT_EQ(r.calls, 2);
}

TEST(Signal, ConnectConstMemberAndLambda) {
    sender s;
    receiver r;
    auto c1 = s.value_changed.connect(&r, &receiver::on_value_const,
                                      utils::connection_type::direct);
    EXPECT_TRUE(c1.connected());
    s.value_changed.emit(2);
    EXPECT_EQ(r.last.load(), 2);

    int captured = 0;
    auto c2 = s.value_changed.connect(&r, [&](int v) { captured = v; },
                                      utils::connection_type::direct);
    EXPECT_TRUE(c2.connected());
    s.value_changed.emit(8);
    EXPECT_EQ(captured, 8);
}

TEST(Signal, UniqueConnectionDoesNotDuplicate) {
    sender s;
    receiver r;
    auto first = s.value_changed.connect(&r, &receiver::on_value,
                                         utils::connection_type::direct,
                                         utils::unique_connection);
    auto second = s.value_changed.connect(&r, &receiver::on_value,
                                          utils::connection_type::direct,
                                          utils::unique_connection);
    EXPECT_TRUE(first.connected());
    EXPECT_FALSE(second.connected());
    s.value_changed.emit(3);
    EXPECT_EQ(r.calls, 1);
}

TEST(Signal, BlockSignalsOnObjectAndSignal) {
    sender s;
    receiver r;
    s.value_changed.connect(&r, &receiver::on_value,
                            utils::connection_type::direct);

    s.block_signals(true);
    s.value_changed.emit(1);
    EXPECT_EQ(r.calls, 0);
    EXPECT_TRUE(s.value_changed.signals_blocked());

    s.block_signals(false);
    s.value_changed.block_signals(true);
    EXPECT_TRUE(s.value_changed.signals_blocked());
    s.value_changed.emit(2);
    EXPECT_EQ(r.calls, 0);

    s.value_changed.block_signals(false);
    EXPECT_FALSE(s.value_changed.signals_blocked());
    s.value_changed.emit(8);
    EXPECT_EQ(r.last, 8);
}

TEST(Signal, DisconnectByConnectionReceiverAndAll) {
    sender s;
    receiver r1;
    receiver r2;
    auto c1 = s.value_changed.connect(&r1, &receiver::on_value,
                                      utils::connection_type::direct);
    auto c2 = s.value_changed.connect(&r2, &receiver::on_value,
                                      utils::connection_type::direct);

    s.value_changed.disconnect(c1);
    EXPECT_FALSE(c1.connected());
    s.value_changed.emit(1);
    EXPECT_EQ(r1.calls, 0);
    EXPECT_EQ(r2.calls, 1);

    s.value_changed.disconnect(&r2);
    s.value_changed.emit(2);
    EXPECT_EQ(r2.calls, 1);

    auto c3 = s.value_changed.connect(&r1, &receiver::on_value,
                                      utils::connection_type::direct);
    s.value_changed.disconnect_all();
    EXPECT_FALSE(c3.connected());
    s.value_changed.emit(3);
    EXPECT_EQ(r1.calls, 0);
}

TEST(Signal, ConnectAndDisconnectSmartPointers) {
    sender s;
    auto up = std::make_unique<receiver>();
    auto sp = std::make_shared<receiver>();
    utils::object_wptr<receiver> wp = sp;

    auto c1 = s.value_changed.connect(up, &receiver::on_value,
                                      utils::connection_type::direct);
    auto c2 = s.value_changed.connect(sp, &receiver::on_value,
                                      utils::connection_type::direct);
    auto c3 = s.value_changed.connect(wp, &receiver::on_value,
                                      utils::connection_type::direct);
    EXPECT_TRUE(c1.connected());
    EXPECT_TRUE(c2.connected());
    EXPECT_TRUE(c3.connected());

    s.value_changed.emit(4);
    EXPECT_EQ(up->last.load(), 4);
    EXPECT_EQ(sp->last.load(), 4);

    int via_up = 0;
    int via_sp = 0;
    int via_wp = 0;
    (void)s.value_changed.connect(up, [&](int v) { via_up = v; },
                                  utils::connection_type::direct);
    (void)s.value_changed.connect(sp, [&](int v) { via_sp = v; },
                                  utils::connection_type::direct);
    (void)s.value_changed.connect(wp, [&](int v) { via_wp = v; },
                                  utils::connection_type::direct);
    s.value_changed.emit(6);
    EXPECT_EQ(via_up, 6);
    EXPECT_EQ(via_sp, 6);
    EXPECT_EQ(via_wp, 6);

    s.value_changed.disconnect(up);
    s.value_changed.disconnect(sp);
    s.value_changed.disconnect(wp);
    up->calls = 0;
    sp->calls = 0;
    s.value_changed.emit(7);
    EXPECT_EQ(up->calls, 0);
    EXPECT_EQ(sp->calls, 0);
}

TEST(Signal, QueuedEmitRunsOnWorkerThread) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

    sender s;
    auto r = std::make_unique<receiver>();
    r->move_to_thread(worker);
    s.value_changed.connect(r.get(), &receiver::on_value,
                            utils::connection_type::queued);
    s.value_changed.emit(42);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 42; }));
    worker.stop();
}

TEST(Signal, BlockingQueuedWaitsForWorker) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    sender s;
    auto r = std::make_unique<receiver>();
    r->move_to_thread(worker);
    s.value_changed.connect(r.get(), &receiver::on_value,
                            utils::connection_type::blocking_queued);
    s.value_changed.emit(7);
    EXPECT_EQ(r->last.load(), 7);
    worker.stop();
}

TEST(Signal, AutoEmitAfterWorkerStopDoesNotRunOnEmitter) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

    sender s;
    auto r = std::make_unique<receiver>();
    r->move_to_thread(worker);
    s.value_changed.connect(r.get(), &receiver::on_value,
                            utils::connection_type::automatic);

    s.value_changed.emit(1);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 1; }));

    worker.stop();
    EXPECT_EQ(r->loop(), nullptr);

    s.value_changed.emit(2);
    EXPECT_EQ(r->last.load(), 1);
    EXPECT_EQ(r->calls, 1);
}

TEST(Signal, BlockingQueuedToIdleMainDoesNotHang) {
    utils::ensure_thread();
    sender s;
    receiver r;
    s.value_changed.connect(&r, &receiver::on_value,
                            utils::connection_type::blocking_queued);

    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    std::atomic<bool> emitted{false};
    worker.loop()->post([&] {
        s.value_changed.emit(5);
        emitted = true;
    });
    ASSERT_TRUE(wait_until([&] { return emitted.load(); }));
    EXPECT_EQ(r.last.load(), 0);
    worker.stop();
}

TEST(Signal, DestroyLaterReceiverInEarlierSlotIsSafe) {
    sender s;
    auto first = std::make_unique<receiver>();
    auto second = std::make_unique<receiver>();
    receiver *second_raw = second.get();
    int first_calls = 0;

    s.value_changed.connect(first.get(), [&](int) {
        ++first_calls;
        second.reset();
    });
    s.value_changed.connect(second_raw, &receiver::on_value);
    s.value_changed.emit(9);
    EXPECT_EQ(first_calls, 1);
}

TEST(Signal, OwnerlessSignalBlockOnlySelf) {
    utils::signal<int> sig{nullptr};
    EXPECT_FALSE(sig.signals_blocked());
    sig.block_signals(true);
    EXPECT_TRUE(sig.signals_blocked());
    sig.block_signals(false);
    EXPECT_FALSE(sig.signals_blocked());
}

// =============================================================================
// invoke（自由函数）
// =============================================================================

TEST(Invoke, MemberSlotDirectAndConst) {
    receiver r;
    auto result = utils::invoke(&r, &receiver::twice, 21);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 42);

    auto result2 = utils::invoke(&r, &receiver::twice_const,
                                 utils::connection_type::direct, 11);
    ASSERT_TRUE(result2);
    EXPECT_EQ(*result2, 22);

    auto result3 =
        utils::invoke(&r, &receiver::twice, utils::connection_type::direct, 5);
    ASSERT_TRUE(result3);
    EXPECT_EQ(*result3, 10);
}

TEST(Invoke, MemberSlotViaSmartPointers) {
    auto up = std::make_unique<receiver>();
    auto sp = std::make_shared<receiver>();

    auto r1 = utils::invoke(up, &receiver::twice, 3);
    ASSERT_TRUE(r1);
    EXPECT_EQ(*r1, 6);

    auto r2 =
        utils::invoke(up, &receiver::twice, utils::connection_type::direct, 4);
    ASSERT_TRUE(r2);
    EXPECT_EQ(*r2, 8);

    auto r3 = utils::invoke(sp, &receiver::twice, 5);
    ASSERT_TRUE(r3);
    EXPECT_EQ(*r3, 10);

    auto r4 =
        utils::invoke(sp, &receiver::twice, utils::connection_type::direct, 6);
    ASSERT_TRUE(r4);
    EXPECT_EQ(*r4, 12);
}

TEST(Invoke, CallableWithResult) {
    receiver r;
    auto result = utils::invoke(&r, utils::connection_type::direct,
                                [](int x) { return x + 1; }, 40);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 41);

    auto up = std::make_unique<receiver>();
    auto r2 = utils::invoke(up, utils::connection_type::direct,
                            [] { return 9; });
    ASSERT_TRUE(r2);
    EXPECT_EQ(*r2, 9);

    auto sp = std::make_shared<receiver>();
    auto r3 = utils::invoke(sp, utils::connection_type::direct,
                            [] { return 8; });
    ASSERT_TRUE(r3);
    EXPECT_EQ(*r3, 8);
}

TEST(Invoke, VoidTaskDirectAndQueued) {
    receiver r;
    int value = 0;
    utils::invoke(&r, [&] { value = 1; }, utils::connection_type::direct);
    EXPECT_EQ(value, 1);

    auto up = std::make_unique<receiver>();
    utils::invoke(up, [&] { value = 2; }, utils::connection_type::direct);
    EXPECT_EQ(value, 2);

    auto sp = std::make_shared<receiver>();
    utils::invoke(sp, [&] { value = 3; }, utils::connection_type::direct);
    EXPECT_EQ(value, 3);

    utils::object_wptr<receiver> wp = sp;
    utils::invoke(wp, [&] { value = 4; }, utils::connection_type::direct);
    EXPECT_EQ(value, 4);
}

TEST(Invoke, VoidTaskQueuedOnWorker) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] {
        auto *loop = worker.loop();
        return loop && loop->is_pumping();
    }));

    auto r = std::make_unique<receiver>();
    r->move_to_thread(worker);
    std::atomic<int> value{0};
    utils::invoke(r.get(), [&] { value = 55; }, utils::connection_type::queued);
    ASSERT_TRUE(wait_until([&] { return value.load() == 55; }));

    utils::invoke(r.get(), [&] { value = 66; },
                  utils::connection_type::blocking_queued);
    EXPECT_EQ(value.load(), 66);
    worker.stop();
}

// =============================================================================
// connect（自由函数）
// =============================================================================

TEST(FreeConnect, MemberSlotOverloads) {
    sender s;
    receiver r;
    auto c = utils::connect(s.value_changed, &r, &receiver::on_value,
                            utils::connection_type::direct);
    EXPECT_TRUE(c.connected());
    s.value_changed.emit(1);
    EXPECT_EQ(r.last, 1);

    auto c_const = utils::connect(s.value_changed, &r, &receiver::on_value_const,
                                  utils::connection_type::direct);
    EXPECT_TRUE(c_const.connected());
}

TEST(FreeConnect, SmartPointerAndLambdaOverloads) {
    sender s;
    auto up = std::make_unique<receiver>();
    auto sp = std::make_shared<receiver>();
    utils::object_wptr<receiver> wp = sp;

    auto c1 = utils::connect(s.value_changed, up, &receiver::on_value,
                             utils::connection_type::direct);
    auto c2 = utils::connect(s.value_changed, sp, &receiver::on_value,
                             utils::connection_type::direct);
    auto c3 = utils::connect(s.value_changed, wp, &receiver::on_value,
                             utils::connection_type::direct);
    EXPECT_TRUE(c1.connected());
    EXPECT_TRUE(c2.connected());
    EXPECT_TRUE(c3.connected());

    int via_raw = 0;
    int via_up = 0;
    int via_sp = 0;
    int via_wp = 0;
    receiver local;
    (void)utils::connect(s.value_changed, &local, [&](int v) { via_raw = v; },
                         utils::connection_type::direct);
    (void)utils::connect(s.value_changed, up, [&](int v) { via_up = v; },
                         utils::connection_type::direct);
    (void)utils::connect(s.value_changed, sp, [&](int v) { via_sp = v; },
                         utils::connection_type::direct);
    (void)utils::connect(s.value_changed, wp, [&](int v) { via_wp = v; },
                         utils::connection_type::direct);

    s.value_changed.emit(10);
    EXPECT_EQ(via_raw, 10);
    EXPECT_EQ(via_up, 10);
    EXPECT_EQ(via_sp, 10);
    EXPECT_EQ(via_wp, 10);
    EXPECT_EQ(up->last.load(), 10);
    EXPECT_EQ(sp->last.load(), 10);
}

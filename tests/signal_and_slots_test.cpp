#include "concurrency/signal_and_slots/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

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
        std::atomic<int> last{0};
        int calls = 0;

        utils::slots_t<> on_value(int value) {
            last = value;
            ++calls;
            return {};
        }

        utils::slots_t<int> twice(int value) { return value * 2; }

        ~receiver() override { invalidate(); }
    };

} // namespace

TEST(EventLoop, ProcessEventsRunsPostedTasks) {
    utils::event_loop loop;
    int value = 0;
    loop.post([&] { value = 1; });
    loop.process_events();
    EXPECT_EQ(value, 1);
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

TEST(EventLoop, DelayedTimerHonorsBudgetAndCancel) {
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

TEST(SignalAndSlots, DirectEmitAndDisconnect) {
    sender s;
    receiver r;
    auto conn = s.value_changed.connect(&r, &receiver::on_value,
        utils::connection_type::direct);
    EXPECT_TRUE(conn.connected());
    s.value_changed.emit(5);
    EXPECT_EQ(r.last, 5);
    EXPECT_EQ(r.calls, 1);

    conn.disconnect();
    EXPECT_FALSE(conn.connected());
    s.value_changed.emit(6);
    EXPECT_EQ(r.last, 5);
    EXPECT_EQ(r.calls, 1);
}

TEST(SignalAndSlots, UniqueConnectionDoesNotDuplicate) {
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

TEST(SignalAndSlots, BlockSignalsOnObjectAndSignal) {
    sender s;
    receiver r;
    s.value_changed.connect(&r, &receiver::on_value,
        utils::connection_type::direct);

    s.block_signals(true);
    s.value_changed.emit(1);
    EXPECT_EQ(r.calls, 0);

    s.block_signals(false);
    s.value_changed.block_signals(true);
    s.value_changed.emit(2);
    EXPECT_EQ(r.calls, 0);

    s.value_changed.block_signals(false);
    s.value_changed.emit(8);
    EXPECT_EQ(r.last, 8);
}

TEST(SignalAndSlots, DisconnectReceiverAndScopedConnection) {
    sender s;
    receiver r;
    s.value_changed.connect(&r, &receiver::on_value,
        utils::connection_type::direct);
    s.value_changed.disconnect(&r);
    s.value_changed.emit(1);
    EXPECT_EQ(r.calls, 0);

    {
        utils::scoped_connection scoped(s.value_changed.connect(
            &r, &receiver::on_value, utils::connection_type::direct));
        s.value_changed.emit(4);
        EXPECT_EQ(r.last, 4);
    }
    s.value_changed.emit(5);
    EXPECT_EQ(r.last, 4);
}

TEST(SignalAndSlots, ReceiverDestructorDropsInboundConnection) {
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

TEST(SignalAndSlots, LambdaSlotAndInvokeReturnValue) {
    sender s;
    receiver r;
    int captured = 0;
    s.value_changed.connect(&r, [&](int value) { captured = value; });
    s.value_changed(9);
    EXPECT_EQ(captured, 9);

    auto result = utils::invoke(&r, &receiver::twice, 21);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 42);
}

TEST(SignalAndSlots, QueuedEmitRunsOnWorkerThread) {
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

TEST(SignalAndSlots, BlockingQueuedWaitsForWorker) {
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
        // 当前线程正在泵本 loop：再 post_blocking 会死锁，必须拒绝
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

TEST(SignalAndSlots, AutoEmitAfterWorkerStopDoesNotRunOnEmitter) {
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

    s.value_changed.emit(2); // 无 loop：应跳过，不得在发射线程直接调槽
    EXPECT_EQ(r->last.load(), 1);
    EXPECT_EQ(r->calls, 1);
}

TEST(SignalAndSlots, BlockingQueuedToIdleMainDoesNotHang) {
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
        s.value_changed.emit(5); // worker → 未泵的主线程：应跳过而非死等
        emitted = true;
    });
    ASSERT_TRUE(wait_until([&] { return emitted.load(); }));
    EXPECT_EQ(r.last.load(), 0);
    worker.stop();
}

TEST(SignalAndSlots, WorkerAffinitySurvivesStopAndRestart) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

    sender s;
    auto r = std::make_unique<receiver>();
    EXPECT_EQ(r->thread(), utils::ensure_thread());

    r->move_to_thread(worker);
    EXPECT_EQ(r->thread(), &worker);
    EXPECT_EQ(r->worker(), &worker);
    s.value_changed.connect(r.get(), &receiver::on_value,
        utils::connection_type::queued);

    s.value_changed.emit(1);
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 1; }));

    worker.stop();
    EXPECT_EQ(r->thread(), &worker); // 句柄仍在
    EXPECT_EQ(r->loop(), nullptr);   // loop 已销毁
    s.value_changed.emit(2); // stop 后无 loop：应安全跳过，不 UAF
    EXPECT_EQ(r->last.load(), 1);

    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    ASSERT_NE(r->loop(), nullptr);
    s.value_changed.emit(3); // 再 start 后动态绑到新 loop，无需重新 move
    ASSERT_TRUE(wait_until([&] { return r->last.load() == 3; }));
    worker.stop();

    r->move_to_thread(nullptr);
    EXPECT_EQ(r->thread(), utils::ensure_thread());
    EXPECT_EQ(r->worker(), nullptr);
    EXPECT_EQ(r->loop(), utils::ensure_thread()->loop());
}

TEST(SignalAndSlots, CurrentThreadStartStopPumpsOnCallerThread) {
    utils::thread *t = utils::ensure_thread();
    ASSERT_NE(t, nullptr);
    ASSERT_NE(t->loop(), nullptr);

    std::atomic<int> value{0};
    t->loop()->post([&] { value = 7; });

    std::thread stopper([t] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        t->stop();
    });
    t->start(); // 本线程阻塞泵，直到 stop
    stopper.join();

    EXPECT_EQ(value.load(), 7);
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

TEST(EventLoop, WorkerStartThenImmediateStopDoesNotHang) {
    for (int i = 0; i < 40; ++i) {
        utils::worker_thread worker;
        worker.start();
        worker.stop();
        EXPECT_EQ(worker.loop(), nullptr);
    }
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

TEST(SignalAndSlots, DestroyLaterReceiverInEarlierSlotIsSafe) {
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

TEST(SignalAndSlots, DeleteLaterAfterWorkerStopDoesNotLeak) {
    utils::worker_thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

    auto obj = utils::make_object_unique<receiver>();
    obj->move_to_thread(worker);
    worker.stop();
    EXPECT_EQ(obj->loop(), nullptr);
    obj.reset();
}

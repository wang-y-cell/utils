#include "concurrency/signal_and_slots/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <memory>
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
  ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));

  sender s;
  auto r = std::make_unique<receiver>();
  r->move_to_thread(worker);
  s.value_changed.connect(r.get(), &receiver::on_value,
                          utils::connection_type::blocking_queued);
  s.value_changed.emit(7);
  EXPECT_EQ(r->last.load(), 7);
  worker.stop();
}

TEST(SignalAndSlots, WorkerAffinitySurvivesStopAndRestart) {
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
  EXPECT_EQ(r->thread(), nullptr);
  s.value_changed.emit(2); // stop 后无 loop：应安全跳过，不 UAF
  EXPECT_EQ(r->last.load(), 1);

  worker.start();
  ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
  ASSERT_NE(r->thread(), nullptr);
  s.value_changed.emit(3); // 再 start 后动态绑到新 loop，无需重新 move
  ASSERT_TRUE(wait_until([&] { return r->last.load() == 3; }));
  worker.stop();
}

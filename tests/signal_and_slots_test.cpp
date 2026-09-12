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
// event_loop / thread（节选）
// =============================================================================

TEST(EventLoop, PostAndProcessEvents) {
    utils::event_loop loop;
    int value = 0;
    EXPECT_TRUE(loop.post([&] { value = 1; }));
    loop.process_events();
    EXPECT_EQ(value, 1);
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

TEST(Thread, CurrentThreadNullOnMain) {
    EXPECT_EQ(utils::current_thread(), nullptr);
}

TEST(Thread, StartStop) {
    utils::thread worker;
    worker.start();
    ASSERT_TRUE(wait_until([&] { return worker.is_running(); }));
    worker.stop();
    EXPECT_FALSE(worker.is_running());
}

// TEST(Thread, ThreadLoop) {
//     utils::thread worker;
//     worker.loop();
// }

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

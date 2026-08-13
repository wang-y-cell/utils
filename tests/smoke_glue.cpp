/**
 * 胶水模块冒烟测试（C++20）
 */

#include "utils/utils.h"
#include "executor/adapters.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace utils;

static void test_expected() {
    result<int> a = result_ok(21);
    auto b = a.transform([](int n) { return n * 2; });
    assert(b && *b == 42);

    result<int> c = result_err(std::errc::invalid_argument);
    auto d = c.or_else([](const std::error_code&) -> result<int> {
        return result_ok(7);
    });
    assert(d && *d == 7);

    expected<void, int> okv;
    assert(okv);
    expected<void, int> bad = unexpected(3);
    assert(!bad && bad.error() == 3);

    auto chained =
        result_ok(2).and_then([](int n) -> result<int> { return result_ok(n + 1); });
    assert(chained.value_or(0) == 3);
}

static void test_scope_guard() {
    int n = 0;
    {
        auto g = make_scope_guard([&] { ++n; });
        assert(n == 0);
    }
    assert(n == 1);

    {
        auto g = make_scope_guard([&] { ++n; });
        g.dismiss();
    }
    assert(n == 1);

    {
        UTILS_DEFER { ++n; };
    }
    assert(n == 2);

    try {
        scope_fail fail{[&] { ++n; }};
        throw std::runtime_error("x");
    } catch (...) {
    }
    assert(n == 3);

    {
        scope_fail fail{[&] { ++n; }};
        scope_success ok{[&] { ++n; }};
    }
    assert(n == 4);  // 仅 success
}

static int add(int a, int b) { return a + b; }

static void test_functional() {
    auto lambda = [](int x) { return x + 1; };
    function_ref<int(int)> ref = lambda;
    assert(ref(41) == 42);

    function_ref<int(int, int)> fp = add;
    assert(fp(20, 22) == 42);

    any_invocable<int()> f = [x = std::make_unique<int>(40)] { return *x + 2; };
    assert(f);
    assert(f() == 42);

    any_invocable<int()> empty;
    bool threw = false;
    try {
        empty();
    } catch (const std::bad_function_call&) {
        threw = true;
    }
    assert(threw);

    std::function<void()> sf = [] {};
    any_invocable<void()> from_sf = std::move(sf);
    assert(from_sf);
    from_sf();
}

static void test_executor() {
    inline_executor inline_ex;
    int x = 0;
    inline_ex.post([&] { x = 1; });
    assert(x == 1);

    thread_pool pool(2);
    auto pool_ex = make_executor(pool);
    std::atomic<int> y{0};
    pool_ex.post([&] { y = 2; });
    any_invocable<void()> move_only = [&] { y = 3; };
    pool_ex.post(std::move(move_only));
    pool.wait();
    assert(y == 3);

    any_executor any = pool_ex;
    any.post([&] { y = 4; });
    pool.wait();
    assert(y == 4);

    worker_thread worker;
    worker.start();
    for (int i = 0; i < 200 && !worker.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    event_loop* wloop = worker.loop();
    assert(wloop != nullptr && worker.is_running());
    auto loop_ex = make_executor(*wloop);
    std::atomic<int> z{0};
    loop_ex.post([&] { z = 9; });
    for (int i = 0; i < 200 && z.load() != 9; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(z == 9);
    worker.stop();
}

static void test_deadline() {
    auto d = deadline::after(std::chrono::milliseconds(30));
    assert(!d.expired());
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(d.expired());
    assert(d.remaining().count() == 0);

    stop_watch sw;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(sw.elapsed_ms() >= 1);
}

static void test_retry() {
    int calls = 0;
    auto r = retry(
        [&]() -> result<int> {
            ++calls;
            if (calls < 3) {
                return result_err(std::errc::connection_reset);
            }
            return result_ok(42);
        },
        retry_policy::fixed(5, std::chrono::milliseconds(1)));
    assert(r && *r == 42);
    assert(calls == 3);

    calls = 0;
    auto fail = retry(
        [&]() -> result<int> {
            ++calls;
            return result_err(std::errc::invalid_argument);
        },
        retry_policy::fixed(3, std::chrono::milliseconds(1)),
        [](const std::error_code& ec) {
            return ec != std::errc::invalid_argument;
        });
    assert(!fail);
    assert(calls == 1);
}

static void test_channel() {
    channel<int> ch(2);
    assert(ch.send(1));
    assert(ch.send(2));
    assert(!ch.try_send(3));  // full

    auto a = ch.recv();
    assert(a && *a == 1);
    assert(ch.try_send(3));

    std::thread producer([&] {
        for (int i = 10; i < 15; ++i) {
            ch.send(i);
        }
        ch.close();
    });

    int sum = 0;
    while (auto v = ch.recv()) {
        sum += *v;
    }
    producer.join();
    // 2,3 left + 10..14
    assert(sum == 2 + 3 + 10 + 11 + 12 + 13 + 14);
}

static void test_config_log() {
    map_config cfg;
    cfg.set("port", static_cast<std::int64_t>(8080));
    cfg.set("db.host", "localhost");
    cfg.set_bool("flag", true);
    assert(cfg.get_int("port") == 8080);
    assert(cfg.get_bool("flag") == true);

    auto db = cfg.section("db");
    auto host = db->get_string("host");
    assert(host && *host == "localhost");

    log::set_backend(std::make_shared<log::null_backend>());
    log::set_level(log::level::info);
    log::info("smoke port={}", 8080);
    log::set_backend(std::make_shared<log::stream_backend>());
}

int main() {
    test_expected();
    test_scope_guard();
    test_functional();
    test_executor();
    test_deadline();
    test_retry();
    test_channel();
    test_config_log();
    std::cout << "smoke_glue: ok\n";
    return 0;
}

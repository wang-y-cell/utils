/**
 * 胶水模块冒烟测试（C++20）
 */

#include "utils/utils.h"
#include "executor/adapters.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace utils;

static void test_expected() {
    Result<int> a = result_ok(21);
    auto b = a.transform([](int n) { return n * 2; });
    assert(b && *b == 42);

    Result<int> c = result_err(std::errc::invalid_argument);
    auto d = c.or_else([](const std::error_code&) -> Result<int> {
        return result_ok(7);
    });
    assert(d && *d == 7);

    Expected<void, int> okv;
    assert(okv);
    Expected<void, int> bad = unexpected(3);
    assert(!bad && bad.error() == 3);

    auto chained =
        result_ok(2).and_then([](int n) -> Result<int> { return result_ok(n + 1); });
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
        ScopeFail fail{[&] { ++n; }};
        throw std::runtime_error("x");
    } catch (...) {
    }
    assert(n == 3);

    {
        ScopeFail fail{[&] { ++n; }};
        ScopeSuccess ok{[&] { ++n; }};
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

static void test_span() {
    std::vector<int> v{1, 2, 3};
    auto s = as_span(v);
    assert(s.size() == 3 && s[0] == 1);

    auto bytes = as_bytes(s);
    assert(bytes.size() == 3 * sizeof(int));

    assert(trim("  hi \n") == "hi");

    int count = 0;
    for (auto part : split("a,b,c", ',')) {
        (void)part;
        ++count;
    }
    assert(count == 3);
}

static void test_executor() {
    InlineExecutor inline_ex;
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

    AnyExecutor any = pool_ex;
    any.post([&] { y = 4; });
    pool.wait();
    assert(y == 4);

    WorkerThread worker;
    worker.start();
    for (int i = 0; i < 200 && !worker.isRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EventLoop* wloop = worker.loop();
    assert(wloop != nullptr && worker.isRunning());
    auto loop_ex = make_executor(*wloop);
    std::atomic<int> z{0};
    loop_ex.post([&] { z = 9; });
    for (int i = 0; i < 200 && z.load() != 9; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(z == 9);
    worker.stop();
}

int main() {
    test_expected();
    test_scope_guard();
    test_functional();
    test_span();
    test_executor();
    std::cout << "smoke_glue: ok\n";
    return 0;
}

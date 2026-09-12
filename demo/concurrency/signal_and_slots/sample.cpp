/**
 * signal_and_slots 用法演示
 * 编译: cmake --build build --target demo_signal
 *
 * 要点:
 * - 接收者必须用 sptr / wptr；用自由函数 connect（不可 signal.connect）
 * - connect(..., thread*) 固定亲和并写入亲和表
 * - 单一 thread（worker）；无 trackable / core_application
 */

#include "concurrency/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

class button {
public:
    signal<> on_clicked;
    signal<std::string> on_double_clicked;
};

class window {
public:
    explicit window(std::string name) : name_(std::move(name)) {}

    void set_hits(std::atomic<int> *hits) { hits_ = hits; }

    slots_t<> on_update_ui() {
        std::cout << "[" << name_ << "] UI refresh @ "
                  << std::this_thread::get_id() << "\n";
        return {};
    }

    slots_t<> on_update_ui_double(const std::string &event_type) {
        std::cout << "[" << name_ << "] double(" << event_type << ") @ "
                  << std::this_thread::get_id() << "\n";
        return {};
    }

    slots_t<> on_hit() {
        if (hits_)
            hits_->fetch_add(1);
        return {};
    }

    slots_t<int> on_name_len() { return static_cast<int>(name_.size()); }

private:
    std::string name_;
    std::atomic<int> *hits_ = nullptr;
};

static void demo_cross_thread() {
    std::cout << "\n=== 1) Auto: 同线程 Direct / 跨线程 Queued ===\n";

    thread worker;
    worker.start();

    button button;
    auto win_ui = std::make_shared<window>("MainWindow");
    auto win_worker = std::make_shared<window>("WorkerWindow");

    scoped_connection c1{
        connect(button.on_clicked, win_ui, &window::on_update_ui)};
    scoped_connection c2{
        connect(button.on_clicked, win_worker, &window::on_update_ui, &worker)};
    connection c3 =
        connect(button.on_double_clicked, win_ui, &window::on_update_ui_double,
                connection_type::direct);

    button.on_clicked.emit();
    button.on_double_clicked.emit("ON_DOUBLE_CLICK");

    std::this_thread::sleep_for(50ms);

    c3.disconnect();
    (void)c1;
    (void)c2;
    worker.stop();
}

static void demo_lifetime() {
    std::cout << "\n=== 2) shared_ptr 释放后自动跳过，不再回调 ===\n";

    button button;
    std::atomic<int> hits{0};

    {
        auto temp = std::make_shared<window>("TempWindow");
        temp->set_hits(&hits);
        scoped_connection sc{connect(button.on_clicked, temp, &window::on_hit)};
        button.on_clicked.emit();
    }

    button.on_clicked.emit();
    std::cout << "hits=" << hits.load() << " (期望 1)\n";
}

static int free_add(int a, int b) { return a + b; }

static void demo_timer_and_invoke() {
    std::cout << "\n=== 3) 定时器 + invoke ===\n";

    thread worker;
    worker.start();

    auto win = std::make_shared<window>("TimerWindow");
    bind_slot_affinity(win.get(), &window::on_name_len, &worker);
    bind_slot_affinity(win.get(), &window::on_update_ui, &worker);

    std::atomic<int> ticks{0};
    auto tid = worker.loop()->post_periodic(30ms, [&] {
        int n = ticks.fetch_add(1) + 1;
        std::cout << "[timer] tick " << n << " @ "
                  << std::this_thread::get_id() << "\n";
    });

    invoke(&worker, [&] {
        std::cout << "[invoke] on worker @ " << std::this_thread::get_id()
                  << "\n";
        win->on_update_ui();
    });

    auto len =
        invoke(win, &window::on_name_len, connection_type::blocking_queued);
    std::cout << "invoke on_name_len=";
    if (len) {
        std::cout << *len << " (期望 " << std::string("TimerWindow").size()
                  << ")\n";
    } else {
        std::cout << "err " << len.error().message() << "\n";
    }

    auto sum =
        invoke(&worker, connection_type::blocking_queued, free_add, 40, 2);
    std::cout << "invoke free_add=";
    if (sum) {
        std::cout << *sum << " (期望 42)\n";
    } else {
        std::cout << "err " << sum.error().message() << "\n";
    }

    auto doubled = invoke(&worker, connection_type::blocking_queued,
                          [](int x) { return x * 2; }, 21);
    std::cout << "invoke lambda=";
    if (doubled) {
        std::cout << *doubled << " (期望 42)\n";
    } else {
        std::cout << "err " << doubled.error().message() << "\n";
    }

    std::this_thread::sleep_for(120ms);
    worker.loop()->cancel_timer(tid);
    std::this_thread::sleep_for(40ms);
    worker.stop();
    std::cout << "timer ticks=" << ticks.load() << " (期望约 3~4)\n";
}

static void demo_blocking_queued() {
    std::cout << "\n=== 4) blocking_queued ===\n";

    thread worker;
    worker.start();

    auto win = std::make_shared<window>("BlockingWindow");

    auto ui = invoke(win, &window::on_update_ui, &worker,
                     connection_type::blocking_queued);
    std::cout << "blocking invoke slot ok=" << static_cast<bool>(ui)
              << " (期望 1)\n";

    button button;
    scoped_connection sc{connect(button.on_clicked, win, &window::on_update_ui,
                                 &worker, connection_type::blocking_queued)};
    button.on_clicked.emit();

    worker.stop();
}

static void demo_scoped_disconnect() {
    std::cout << "\n=== 5) scoped_connection RAII ===\n";

    button button;
    auto win = std::make_shared<window>("ScopedWindow");
    std::atomic<int> hits{0};
    win->set_hits(&hits);

    {
        scoped_connection sc{connect(button.on_clicked, win, &window::on_hit)};
        button.on_clicked.emit();
    }

    button.on_clicked.emit();
    std::cout << "hits=" << hits.load() << " (期望 1)\n";
}

static void demo_unique_block_disconnect() {
    std::cout << "\n=== 6) unique / block_signals / disconnect(sptr) ===\n";

    button btn;
    auto win = std::make_shared<window>("UniqueWindow");
    std::atomic<int> hits{0};
    win->set_hits(&hits);

    auto c1 = connect(btn.on_clicked, win, &window::on_update_ui,
                      connection_type::automatic, unique_connection);
    auto c2 = connect(btn.on_clicked, win, &window::on_update_ui,
                      connection_type::automatic, unique_connection);
    std::cout << "unique second connected=" << c2.connected() << " (期望 0)\n";

    (void)connect(btn.on_clicked, win, &window::on_hit);
    btn.on_clicked.block_signals(true);
    btn.on_clicked.emit();
    std::cout << "blocked hits=" << hits.load() << " (期望 0)\n";

    btn.on_clicked.block_signals(false);
    btn.on_clicked.emit();
    std::cout << "unblocked hits=" << hits.load() << " (期望 1)\n";

    btn.on_clicked.disconnect(win);
    btn.on_clicked.emit();
    std::cout << "after disconnect(sptr) hits=" << hits.load() << " (期望 1)\n";
    (void)c1;
}

static void demo_lambda_connect() {
    std::cout << "\n=== 7) connect(sptr, lambda) ===\n";

    button btn;
    auto win = std::make_shared<window>("LambdaWindow");
    std::atomic<int> hits{0};

    scoped_connection sc{connect(btn.on_clicked, win, [&] {
        hits.fetch_add(1);
        std::cout << "[lambda] hit @ " << std::this_thread::get_id() << "\n";
    })};

    scoped_connection sc2{connect(btn.on_double_clicked, win,
                                  [&](const std::string &s) {
                                      std::cout << "[lambda] double args=" << s
                                                << "\n";
                                  })};

    btn.on_clicked.emit();
    btn.on_double_clicked.emit("from-lambda");
    std::cout << "lambda hits=" << hits.load() << " (期望 1)\n";
}

static void demo_per_slot_threads() {
    std::cout << "\n=== 8) 同一对象不同槽绑不同 thread ===\n";

    thread w1;
    thread w2;
    w1.start();
    w2.start();

    auto win = std::make_shared<window>("MultiThreadWindow");
    signal<> ping_a;
    signal<> ping_b;

    (void)connect(ping_a, win, &window::on_hit, &w1, connection_type::queued);
    bind_slot_affinity(win.get(), &window::on_update_ui, &w2);
    (void)connect(ping_b, win, &window::on_update_ui, connection_type::queued);

    std::cout << "slot_a affinity="
              << (find_slot_affinity(win.get(), &window::on_hit) == &w1)
              << "\n";
    std::cout << "slot_b affinity="
              << (find_slot_affinity(win.get(), &window::on_update_ui) == &w2)
              << "\n";

    ping_a.emit();
    ping_b.emit();
    std::this_thread::sleep_for(50ms);
    w1.stop();
    w2.stop();
}

int main() {
    demo_cross_thread();
    demo_lifetime();
    demo_timer_and_invoke();
    demo_blocking_queued();
    demo_scoped_disconnect();
    demo_unique_block_disconnect();
    demo_lambda_connect();
    demo_per_slot_threads();
    std::cout << "\n全部示例结束。\n";
    return 0;
}

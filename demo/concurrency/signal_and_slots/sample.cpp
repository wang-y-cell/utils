/**
* signal_and_slots 用法演示
* 编译: cmake --build build --target demo_signal
*
* 要点:
* - 成员槽必须返回 slots_t / slots_t<T>；也可 connect(receiver, lambda)
* - 只有 invoke(成员槽) 能 get 到值（Direct / BlockingQueued）
* - 槽接收者继承 object；派生析构第一行 invalidate()（同亲和会排空队列）
* - connection_type: Direct / Queued / BlockingQueued / Auto
* - unique 连接 / block_signals / disconnect(receiver)
* - scoped_connection RAII；worker_thread + invoke / 定时器
* - 禁止在工作线程内调用 worker_thread::stop()
*/

#include "concurrency/signal_and_slots/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

class button : public object {
    public:
        signal<> on_clicked{this};
        signal<std::string> on_double_clicked{this};
};

class window : public object {
    public:
        explicit window(std::string name) : name_(std::move(name)) {}

        // 派生析构第一行必须 invalidate（断连 + 同亲和 process_events 排空）
        ~window() override { invalidate(); }

        void set_hits(std::atomic<int>* hits) { hits_ = hits; }

        slots_t<> on_update_ui() {
            std::cout << "[" << name_ << "] UI refresh @ "
            << std::this_thread::get_id() << "\n";
            return {};
        }

        slots_t<> on_update_ui_double(const std::string& event_type) {
            std::cout << "[" << name_ << "] double(" << event_type << ") @ "
            << std::this_thread::get_id() << "\n";
            return {};
        }

        slots_t<> on_hit() {
            if (hits_) hits_->fetch_add(1);
            return {};
        }

        slots_t<int> on_name_len() { return static_cast<int>(name_.size()); }

    private:
        std::string name_;
        std::atomic<int>* hits_ = nullptr;
};

static void demo_cross_thread() {
    std::cout << "\n=== 1) Auto: 同线程 Direct / 跨线程 Queued ===\n";

    worker_thread worker;
    worker.start();

    button button;
    window win_ui("MainWindow");
    window win_worker("WorkerWindow");

    win_worker.move_to_thread(worker);

    scoped_connection c1{
        connect(button.on_clicked, &win_ui, &window::on_update_ui)};
    scoped_connection c2{
        button.on_clicked.connect(&win_worker, &window::on_update_ui)};
    connection c3 = connect(button.on_double_clicked, &win_ui,
        &window::on_update_ui_double, connection_type::direct);

    button.on_clicked.emit();
    button.on_double_clicked.emit("ON_DOUBLE_CLICK");

    std::this_thread::sleep_for(50ms);

    c3.disconnect();
    (void)c1;
    (void)c2;
    worker.stop();
}

static void demo_lifetime() {
    std::cout << "\n=== 2) 接收者析构后自动断开，不再回调 ===\n";

    button button;
    std::atomic<int> hits{0};

    {
        window temp("TempWindow");
        temp.set_hits(&hits);
        button.on_clicked.connect(&temp, &window::on_hit);
        button.on_clicked.emit();
    }

    button.on_clicked.emit();
    std::cout << "hits=" << hits.load() << " (期望 1)\n";
}

static int free_add(int a, int b) { return a + b; }

static void demo_timer_and_invoke() {
    std::cout << "\n=== 3) 定时器 + invoke ===\n";

    worker_thread worker;
    worker.start();

    window win("TimerWindow");
    win.move_to_thread(worker);

    std::atomic<int> ticks{0};

    auto tid = worker.loop()->post_periodic(30ms, [&] {
        int n = ticks.fetch_add(1) + 1;
        std::cout << "[timer] tick " << n << " @ "
        << std::this_thread::get_id() << "\n";
    });

    invoke(&win, [&] {
        std::cout << "[invoke] on worker @ " << std::this_thread::get_id()
        << "\n";
        win.on_update_ui();
    });

    auto len = invoke(&win, &window::on_name_len,
        connection_type::blocking_queued);
    std::cout << "invoke on_name_len=";
    if (len) {
        std::cout << *len << " (期望 " << std::string("TimerWindow").size()
        << ")\n";
    } else {
        std::cout << "err " << len.error().message() << "\n";
    }

    auto sum = invoke(&win, connection_type::blocking_queued, free_add, 40, 2);
    std::cout << "invoke free_add=";
    if (sum) {
        std::cout << *sum << " (期望 42)\n";
    } else {
        std::cout << "err " << sum.error().message() << "\n";
    }

    auto doubled = invoke(&win, connection_type::blocking_queued,
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
    std::cout << "\n=== 4) blocking_queued：等待目标线程执行完成 ===\n";

    worker_thread worker;
    worker.start();

    window win("BlockingWindow");
    win.move_to_thread(worker);

    auto ui = invoke(&win, &window::on_update_ui,
        connection_type::blocking_queued);
    std::cout << "blocking invoke slot ok=" << static_cast<bool>(ui)
    << " (期望 1)\n";

    button button;
    button.on_clicked.connect(&win, &window::on_update_ui,
        connection_type::blocking_queued);
    button.on_clicked.emit();

    worker.stop();
}

static void demo_scoped_disconnect() {
    std::cout << "\n=== 5) scoped_connection RAII ===\n";

    button button;
    window win("ScopedWindow");
    std::atomic<int> hits{0};
    win.set_hits(&hits);

    {
        scoped_connection sc{
            button.on_clicked.connect(&win, &window::on_hit)};
        button.on_clicked.emit();
    }

    button.on_clicked.emit();
    std::cout << "hits=" << hits.load() << " (期望 1)\n";
}

static void demo_unique_block_disconnect() {
    std::cout << "\n=== 6) unique / block_signals / disconnect(receiver) ===\n";

    button btn;
    window win("UniqueWindow");
    std::atomic<int> hits{0};
    win.set_hits(&hits);

    auto c1 = connect(btn.on_clicked, &win, &window::on_update_ui,
        connection_type::automatic, unique_connection);
    auto c2 = connect(btn.on_clicked, &win, &window::on_update_ui,
        connection_type::automatic, unique_connection);
    std::cout << "unique second connected=" << c2.connected() << " (期望 0)\n";

    btn.on_clicked.connect(&win, &window::on_hit);
    btn.block_signals(true);
    btn.on_clicked.emit();
    std::cout << "blocked hits=" << hits.load() << " (期望 0)\n";

    btn.block_signals(false);
    btn.on_clicked.emit();
    std::cout << "unblocked hits=" << hits.load() << " (期望 1)\n";

    btn.on_clicked.disconnect(&win);
    btn.on_clicked.emit();
    std::cout << "after disconnect(receiver) hits=" << hits.load()
    << " (期望 1)\n";
    (void)c1;
}

static void demo_lambda_connect() {
    std::cout << "\n=== 7) connect(receiver, lambda) ===\n";

    button btn;
    window win("LambdaWindow");
    std::atomic<int> hits{0};

    scoped_connection sc{connect(btn.on_clicked, &win, [&] {
            hits.fetch_add(1);
            std::cout << "[lambda] hit @ " << std::this_thread::get_id() << "\n";
        })};

    scoped_connection sc2{btn.on_double_clicked.connect(
        &win, [&](const std::string& s) {
            std::cout << "[lambda] double args=" << s << "\n";
        })};

    btn.on_clicked.emit();
    btn.on_double_clicked.emit("from-lambda");
    std::cout << "lambda hits=" << hits.load() << " (期望 1)\n";
}

int main() {
    core_application app;  // 主线程注册 ensure_thread() + 默认 event_loop

    demo_cross_thread();
    demo_lifetime();
    demo_timer_and_invoke();
    demo_blocking_queued();
    demo_scoped_disconnect();
    demo_unique_block_disconnect();
    demo_lambda_connect();
    std::cout << "\n全部示例结束。\n";
    return 0;
}

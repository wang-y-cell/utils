/**
 * signal_and_slots 用法演示
 * 编译: cmake --build build --target demo_signal
 *
 * 要点:
 * - 槽接收者继承 object，才能 Queued/Auto 跨线程与析构自动断连
 * - connection_type: Direct / Queued / BlockingQueued / Auto
 * - scoped_connection RAII；worker_thread + invoke / 定时器
 */

#include "component/signal_and_slots/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

class button : public object {
public:
    signal<> on_clicked;
    signal<std::string> on_double_clicked;

};

class window : public object {
public:
    explicit window(std::string name) : name_(std::move(name)) {}

    ~window() override { invalidate(); }

    void on_update_ui() {
        std::cout << "[" << name_ << "] UI refresh @ "
                  << std::this_thread::get_id() << "\n";
    }

    void on_update_ui_double(const std::string& event_type) {
        std::cout << "[" << name_ << "] double(" << event_type << ") @ "
                  << std::this_thread::get_id() << "\n";
    }

private:
    std::string name_;
};

static void demo_cross_thread() {
    std::cout << "\n=== 1) Auto: 同线程 Direct / 跨线程 Queued ===\n";

    worker_thread worker;
    worker.start();

    // 无需 main_marker：object 构造已绑定主线程默认 loop
    button button;
    window win_ui("MainWindow");
    window win_worker("WorkerWindow");

    win_worker.move_to_thread(worker.loop());

    // connect 语法糖：成员函数指针
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
    (void)c1; //防止编译器警告
    (void)c2; //防止编译器警告
    worker.stop();
}

static void demo_lifetime() {
    std::cout << "\n=== 2) 接收者析构后自动断开，不再回调 ===\n";

    button button;
    std::atomic<int> hits{0};

    {
        window temp("TempWindow");
        button.on_clicked.connect(&temp, [&] {
            hits.fetch_add(1);
            temp.on_update_ui();
        });
        button.on_clicked.emit();
    }

    button.on_clicked.emit();
    std::cout << "hits=" << hits.load() << " (期望 1)\n";
}

static void demo_timer_and_invoke() {
    std::cout << "\n=== 3) 定时器 + invoke ===\n";

    worker_thread worker;
    worker.start();

    window win("TimerWindow");
    win.move_to_thread(worker.loop());

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
    win.move_to_thread(worker.loop());

    std::atomic<bool> done{false};
    const auto caller = std::this_thread::get_id();
    invoke(&win, [&] {
        done.store(true, std::memory_order_release);
        std::cout << "[blocking invoke] worker @ " << std::this_thread::get_id()
                  << ", caller @ " << caller << "\n";
    }, connection_type::blocking_queued);

    std::cout << "done after invoke=" << done.load(std::memory_order_acquire)
              << " (期望 1)\n";

    button button;
    button.on_clicked.connect(&win, [&] {
        done.store(true, std::memory_order_release);
        win.on_update_ui();
    }, connection_type::blocking_queued);
    done.store(false, std::memory_order_release);
    button.on_clicked.emit();
    std::cout << "done after emit=" << done.load(std::memory_order_acquire)
              << " (期望 1)\n";

    worker.stop();
}

static void demo_scoped_disconnect() {
    std::cout << "\n=== 5) scoped_connection RAII ===\n";

    button button;
    std::atomic<int> hits{0};

    {
        scoped_connection sc{
            button.on_clicked.connect([&] { hits.fetch_add(1); })};
            button.on_clicked.emit();
    }

    button.on_clicked.emit();
    std::cout << "hits=" << hits.load() << " (期望 1)\n";
}

int main() {
    core_application app;  // 主线程注册默认 event_loop（仿 QCoreApplication）

    demo_cross_thread();
    demo_lifetime();
    demo_timer_and_invoke();
    demo_blocking_queued();
    demo_scoped_disconnect();
    std::cout << "\n全部示例结束。\n";
    return 0;
}

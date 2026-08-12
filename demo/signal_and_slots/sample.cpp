/**
 * signal_and_slots 用法演示
 * 编译: cmake --build build --target demo_signal
 *
 * 要点:
 * - 槽接收者继承 Object，才能 Queued/Auto 跨线程与析构自动断连
 * - ConnectionType: Direct / Queued / Auto
 * - ScopedConnection RAII；WorkerThread + invoke / 定时器
 */

#include "signal_and_slots/signal_and_slots.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace utils;
using namespace std::chrono_literals;

class Button : public Object {
public:
    Signal<> onClicked;
    Signal<std::string> onDoubleClicked;

};

class Window : public Object {
public:
    explicit Window(std::string name) : name_(std::move(name)) {}

    ~Window() override { invalidate(); }

    void onUpdateUI() {
        std::cout << "[" << name_ << "] UI refresh @ "
                  << std::this_thread::get_id() << "\n";
    }

    void onUpdateUIDouble(const std::string& event_type) {
        std::cout << "[" << name_ << "] double(" << event_type << ") @ "
                  << std::this_thread::get_id() << "\n";
    }

private:
    std::string name_;
};

static void demo_cross_thread() {
    std::cout << "\n=== 1) Auto: 同线程 Direct / 跨线程 Queued ===\n";

    WorkerThread worker;
    worker.start();

    // 无需 main_marker：Object 构造已绑定主线程默认 loop
    Button button;
    Window win_ui("MainWindow");
    Window win_worker("WorkerWindow");

    win_worker.moveToThread(worker.loop());

    // connect 语法糖：成员函数指针
    ScopedConnection c1{
        connect(button.onClicked, &win_ui, &Window::onUpdateUI)};
    ScopedConnection c2{
        button.onClicked.connect(&win_worker, &Window::onUpdateUI)};
    Connection c3 = connect(button.onDoubleClicked, &win_ui,
                            &Window::onUpdateUIDouble, ConnectionType::Direct);

    
    button.onClicked.emit();
    button.onDoubleClicked.emit("ON_DOUBLE_CLICK");

    std::this_thread::sleep_for(50ms);

    c3.disconnect();
    (void)c1; //防止编译器警告
    (void)c2; //防止编译器警告
    worker.stop();
}

static void demo_lifetime() {
    std::cout << "\n=== 2) 接收者析构后自动断开，不再回调 ===\n";

    Button button;
    std::atomic<int> hits{0};

    {
        Window temp("TempWindow");
        button.onClicked.connect(&temp, [&] {
            hits.fetch_add(1);
            temp.onUpdateUI();
        });
        button.onClicked.emit();
    }

    button.onClicked.emit();
    std::cout << "hits=" << hits.load() << " (期望 1)\n";
}

static void demo_timer_and_invoke() {
    std::cout << "\n=== 3) 定时器 + invoke ===\n";

    WorkerThread worker;
    worker.start();

    Window win("TimerWindow");
    win.moveToThread(worker.loop());

    std::atomic<int> ticks{0};

    auto tid = worker.loop()->postPeriodic(30ms, [&] {
        int n = ticks.fetch_add(1) + 1;
        std::cout << "[timer] tick " << n << " @ "
                  << std::this_thread::get_id() << "\n";
    });

    invoke(&win, [&] {
        std::cout << "[invoke] on worker @ " << std::this_thread::get_id()
                  << "\n";
        win.onUpdateUI();
    });

    std::this_thread::sleep_for(120ms);
    worker.loop()->cancelTimer(tid);
    std::this_thread::sleep_for(40ms);
    worker.stop();
    std::cout << "timer ticks=" << ticks.load() << " (期望约 3~4)\n";
}

static void demo_scoped_disconnect() {
    std::cout << "\n=== 4) ScopedConnection RAII ===\n";

    Button button;
    std::atomic<int> hits{0};

    {
        ScopedConnection sc{
            button.onClicked.connect([&] { hits.fetch_add(1); })};
            button.onClicked.emit();
    }

    button.onClicked.emit();
    std::cout << "hits=" << hits.load() << " (期望 1)\n";
}

int main() {
    CoreApplication app;  // 主线程注册默认 EventLoop（仿 QCoreApplication）

    demo_cross_thread();
    demo_lifetime();
    demo_timer_and_invoke();
    demo_scoped_disconnect();
    std::cout << "\n全部示例结束。\n";
    return 0;
}

/**
 * Logging facade 用法演示
 * 编译: cmake --build build --target demo_log
 *
 * 要点:
 * - 业务只调 log::info/warn/error，不依赖具体日志库
 * - set_backend 切换 StreamBackend / NullBackend / 自研 / spdlog 适配器
 * - set_level 过滤低级别日志
 */

#include "log/log.h"

#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using namespace utils;

int main() {
    std::cout << "=== 1) 默认 StreamBackend → cerr ===\n";
    log::set_level(log::Level::Debug);
    log::debug("debug detail={}", 1);
    log::info("server listen port={}", 8080);
    log::warn("disk almost full percent={}", 92);
    log::error("request failed code={}", 500);

    std::cout << "\n=== 2) 级别过滤 ===\n";
    log::set_level(log::Level::Warn);
    log::info("this should be hidden");
    log::warn("this is visible");

    std::cout << "\n=== 3) NullBackend（测试静默） ===\n";
    log::set_backend(std::make_shared<log::NullBackend>());
    log::error("should not appear");
    std::cout << "  (no log lines above expected)\n";

    std::cout << "\n=== 4) 自定义后端示例 ===\n";
    struct CaptureBackend : log::Backend {
        std::string last;
        void log(log::Level, std::string_view message) override {
            last = std::string(message);
        }
    };
    auto cap = std::make_shared<CaptureBackend>();
    log::set_backend(cap);
    log::set_level(log::Level::Info);
    log::info("captured={}", 7);
    std::cout << "  captured='" << cap->last << "'\n";

    // 恢复默认，避免影响同进程其它测试
    log::set_backend(std::make_shared<log::StreamBackend>());
    log::set_level(log::Level::Info);

    std::cout << "\ndemo_log: ok\n";
    return 0;
}

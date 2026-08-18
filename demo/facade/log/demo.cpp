#include "facade/log/log.h"

#include <iostream>
#include <memory>
#include <string>

using namespace utils;

int main() {
    log::set_level(log::level::debug);
    log::debug("debug detail={}", 1);
    log::info("server listen port={}", 8080);
    log::warn("disk almost full percent={}", 92);

    struct capture_backend : log::backend {
        std::string last;
        void log(log::level, std::string_view message) override {
            last = std::string(message);
        }
    };

    auto capture = std::make_shared<capture_backend>();
    log::set_backend(capture);
    log::info("captured={}", 7);
    std::cout << "captured='" << capture->last << "'\n";

    log::set_backend(std::make_shared<log::stream_backend>());
    log::set_level(log::level::info);
}

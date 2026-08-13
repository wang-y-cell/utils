/**
 * config_view 用法演示
 * 编译: cmake --build build --target demo_config
 *
 * 要点:
 * - map_config: 内存字典，适合默认配置 / 单测
 * - section("db") 读取带 "db." 前缀的键
 * - set_bool: 布尔请用 set_bool（避免 const char* 匹配 bool）
 * - env_config: 环境变量前缀适配，不绑 JSON 库
 */

#include "adapter/config/config_view.h"

#include <cstdint>
#include <iostream>

using namespace utils;

int main() {
    std::cout << "=== 1) map_config ===\n";
    map_config cfg;
    cfg.set("port", static_cast<std::int64_t>(8080));
    cfg.set("name", "demo-app");
    cfg.set_bool("debug", true);
    cfg.set("db.host", "127.0.0.1");
    cfg.set("db.port", static_cast<std::int64_t>(5432));

    std::cout << "  port=" << cfg.get_int("port").value_or(-1) << '\n';
    std::cout << "  name=" << cfg.get_string("name").value_or("-") << '\n';
    std::cout << "  debug=" << cfg.get_bool("debug").value_or(false) << '\n';

    std::cout << "\n=== 2) section ===\n";
    auto db = cfg.section("db");
    std::cout << "  db.host=" << db->get_string("host").value_or("-") << '\n';
    std::cout << "  db.port=" << db->get_int("port").value_or(-1) << '\n';

    std::cout << "\n=== 3) env_config（无对应环境变量则空） ===\n";
    env_config env("UTILS_DEMO_");
    auto v = env.get_string("port");
    std::cout << "  UTILS_DEMO_PORT set? " << (v ? *v : std::string{"(no)"})
              << '\n';

    std::cout << "\ndemo_config: ok\n";
    return 0;
}

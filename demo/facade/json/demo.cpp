#include "facade/json/json.h"

#include <cstdint>
#include <iostream>

int main() {
    auto document =
        utils::json::parse(R"({"server":{"host":"localhost"}})");
    if (!document) {
        std::cerr << "invalid JSON\n";
        return 1;
    }

    auto host = document->get("server.host");
    if (!host) return 1;
    std::cout << "host=" << host->as_string().value() << '\n';

    if (!document->set("server.port",
                       utils::json::value(std::int64_t{8080}))) {
        return 1;
    }

    auto output = utils::json::stringify(*document, true);
    if (!output) return 1;
    std::cout << *output << '\n';
}

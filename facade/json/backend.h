#pragma once

#include "facade/json/value.h"

#include <string>
#include <string_view>

namespace utils::json {

class backend {
public:
    virtual ~backend() = default;

    virtual result<value> parse(std::string_view text) = 0;
    virtual result<std::string> stringify(const value& input,
                                          bool pretty) = 0;
};

}  // namespace utils::json

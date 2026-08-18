#pragma once

#include "facade/json/backend.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace utils::json {
namespace detail {

inline constexpr std::size_t max_json_depth = 256;

template <class T>
[[nodiscard]] result<T> json_error() {
    return result_err(std::errc::invalid_argument);
}

inline bool is_continuation(unsigned char byte) noexcept {
    return byte >= 0x80 && byte <= 0xBF;
}

inline bool copy_utf8_sequence(std::string_view input, std::size_t start,
                               std::size_t& next, std::string& output) {
    const auto first = static_cast<unsigned char>(input[start]);
    std::size_t length = 0;

    if (first >= 0xC2 && first <= 0xDF) {
        length = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
        length = 3;
    } else if (first >= 0xF0 && first <= 0xF4) {
        length = 4;
    } else {
        return false;
    }

    if (start + length > input.size()) return false;
    for (std::size_t i = 1; i < length; ++i) {
        if (!is_continuation(static_cast<unsigned char>(input[start + i]))) {
            return false;
        }
    }

    const auto second = static_cast<unsigned char>(input[start + 1]);
    if ((first == 0xE0 && second < 0xA0) ||
        (first == 0xED && second > 0x9F) ||
        (first == 0xF0 && second < 0x90) ||
        (first == 0xF4 && second > 0x8F)) {
        return false;
    }

    output.append(input.substr(start, length));
    next = start + length;
    return true;
}

inline bool append_code_point(std::uint32_t code_point,
                              std::string& output) {
    if (code_point <= 0x7F) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        if (code_point >= 0xD800 && code_point <= 0xDFFF) return false;
        output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        output.push_back(
            static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(
            static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(
            static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        return false;
    }
    return true;
}

class parser {
public:
    explicit parser(std::string_view text) : text_(text) {}

    [[nodiscard]] result<value> parse_document() {
        skip_whitespace();
        auto output = parse_value(0);
        if (!output) return output;
        skip_whitespace();
        if (position_ != text_.size()) return json_error<value>();
        return output;
    }

private:
    [[nodiscard]] result<value> parse_value(std::size_t depth) {
        skip_whitespace();
        if (position_ >= text_.size()) return json_error<value>();

        switch (text_[position_]) {
            case '{':
                if (depth >= max_json_depth) return json_error<value>();
                return parse_object(depth + 1);
            case '[':
                if (depth >= max_json_depth) return json_error<value>();
                return parse_array(depth + 1);
            case '"': {
                auto string_value = parse_string();
                if (!string_value) return json_error<value>();
                return result_ok(value(std::move(*string_value)));
            }
            case 't':
                return parse_literal("true", value(true));
            case 'f':
                return parse_literal("false", value(false));
            case 'n':
                return parse_literal("null", value(nullptr));
            default:
                if (text_[position_] == '-' ||
                    (text_[position_] >= '0' && text_[position_] <= '9')) {
                    return parse_number();
                }
                return json_error<value>();
        }
    }

    [[nodiscard]] result<value> parse_object(std::size_t depth) {
        ++position_;
        value::object_type output;
        skip_whitespace();
        if (consume('}')) return result_ok(value(std::move(output)));

        for (;;) {
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_] != '"') {
                return json_error<value>();
            }
            auto key = parse_string();
            if (!key) return json_error<value>();

            skip_whitespace();
            if (!consume(':')) return json_error<value>();
            auto child = parse_value(depth);
            if (!child) return json_error<value>();
            output.insert_or_assign(std::move(*key), std::move(*child));

            skip_whitespace();
            if (consume('}')) break;
            if (!consume(',')) return json_error<value>();
        }
        return result_ok(value(std::move(output)));
    }

    [[nodiscard]] result<value> parse_array(std::size_t depth) {
        ++position_;
        value::array_type output;
        skip_whitespace();
        if (consume(']')) return result_ok(value(std::move(output)));

        for (;;) {
            auto child = parse_value(depth);
            if (!child) return json_error<value>();
            output.push_back(std::move(*child));

            skip_whitespace();
            if (consume(']')) break;
            if (!consume(',')) return json_error<value>();
        }
        return result_ok(value(std::move(output)));
    }

    [[nodiscard]] result<std::string> parse_string() {
        if (!consume('"')) return json_error<std::string>();

        std::string output;
        while (position_ < text_.size()) {
            const std::size_t start = position_;
            const auto byte =
                static_cast<unsigned char>(text_[position_++]);
            if (byte == '"') return result_ok(std::move(output));
            if (byte < 0x20) return json_error<std::string>();

            if (byte >= 0x80) {
                std::size_t next = position_;
                if (!copy_utf8_sequence(text_, start, next, output)) {
                    return json_error<std::string>();
                }
                position_ = next;
                continue;
            }

            if (byte != '\\') {
                output.push_back(static_cast<char>(byte));
                continue;
            }
            if (position_ >= text_.size()) return json_error<std::string>();

            const char escape = text_[position_++];
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    output.push_back(escape);
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    auto code = parse_hex_quad();
                    if (!code) return json_error<std::string>();
                    std::uint32_t code_point = *code;
                    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                        if (position_ + 2 > text_.size() ||
                            text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u') {
                            return json_error<std::string>();
                        }
                        position_ += 2;
                        auto low = parse_hex_quad();
                        if (!low || *low < 0xDC00 || *low > 0xDFFF) {
                            return json_error<std::string>();
                        }
                        code_point =
                            0x10000 + ((code_point - 0xD800) << 10) +
                            (*low - 0xDC00);
                    } else if (code_point >= 0xDC00 &&
                               code_point <= 0xDFFF) {
                        return json_error<std::string>();
                    }
                    if (!append_code_point(code_point, output)) {
                        return json_error<std::string>();
                    }
                    break;
                }
                default:
                    return json_error<std::string>();
            }
        }
        return json_error<std::string>();
    }

    [[nodiscard]] result<std::uint32_t> parse_hex_quad() {
        if (position_ + 4 > text_.size()) {
            return json_error<std::uint32_t>();
        }

        std::uint32_t output = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            output <<= 4;
            if (c >= '0' && c <= '9') {
                output |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                output |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                output |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return json_error<std::uint32_t>();
            }
        }
        return result_ok(output);
    }

    [[nodiscard]] result<value> parse_number() {
        const std::size_t begin = position_;
        if (text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) return json_error<value>();

        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() && text_[position_] >= '0' &&
                text_[position_] <= '9') {
                return json_error<value>();
            }
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') {
                return json_error<value>();
            }
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }

        bool floating = false;
        if (position_ < text_.size() && text_[position_] == '.') {
            floating = true;
            ++position_;
            if (position_ >= text_.size() || text_[position_] < '0' ||
                text_[position_] > '9') {
                return json_error<value>();
            }
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }

        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            floating = true;
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= text_.size() || text_[position_] < '0' ||
                text_[position_] > '9') {
                return json_error<value>();
            }
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }

        const std::string_view token =
            text_.substr(begin, position_ - begin);
        if (!floating) {
            std::int64_t integer = 0;
            const auto converted = std::from_chars(
                token.data(), token.data() + token.size(), integer);
            if (converted.ec == std::errc{} &&
                converted.ptr == token.data() + token.size()) {
                return result_ok(value(integer));
            }
            if (converted.ec != std::errc::result_out_of_range) {
                return json_error<value>();
            }
        }

        double number = 0.0;
        const auto converted = std::from_chars(
            token.data(), token.data() + token.size(), number,
            std::chars_format::general);
        if (converted.ec != std::errc{} ||
            converted.ptr != token.data() + token.size() ||
            !std::isfinite(number)) {
            return json_error<value>();
        }
        return result_ok(value(number));
    }

    [[nodiscard]] result<value> parse_literal(std::string_view literal,
                                              value output) {
        if (text_.substr(position_, literal.size()) != literal) {
            return json_error<value>();
        }
        position_ += literal.size();
        return result_ok(std::move(output));
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++position_;
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

class serializer {
public:
    explicit serializer(bool pretty) : pretty_(pretty) {}

    [[nodiscard]] result<std::string> run(const value& input) {
        if (!append_value(input, 0)) return json_error<std::string>();
        return result_ok(std::move(output_));
    }

private:
    bool append_value(const value& input, std::size_t depth) {
        switch (input.type()) {
            case kind::null:
                output_ += "null";
                return true;
            case kind::boolean:
                output_ += input.as_bool().value() ? "true" : "false";
                return true;
            case kind::integer:
                return append_integer(input.as_int().value());
            case kind::number:
                return append_number(input.as_double().value());
            case kind::string:
                return append_string(input.as_string().value());
            case kind::array:
                if (depth >= max_json_depth) return false;
                return append_array(*input.as_array(), depth);
            case kind::object:
                if (depth >= max_json_depth) return false;
                return append_object(*input.as_object(), depth);
        }
        return false;
    }

    bool append_integer(std::int64_t input) {
        char buffer[32];
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer),
                                             input);
        if (converted.ec != std::errc{}) return false;
        output_.append(buffer, converted.ptr);
        return true;
    }

    bool append_number(double input) {
        if (!std::isfinite(input)) return false;
        char buffer[64];
        const auto converted =
            std::to_chars(buffer, buffer + sizeof(buffer), input,
                          std::chars_format::general);
        if (converted.ec != std::errc{}) return false;
        const std::string_view encoded(
            buffer, static_cast<std::size_t>(converted.ptr - buffer));
        output_.append(buffer, converted.ptr);
        if (encoded.find_first_of(".eE") == std::string_view::npos) {
            output_ += ".0";
        }
        return true;
    }

    bool append_string(std::string_view input) {
        static constexpr char hex[] = "0123456789abcdef";
        output_.push_back('"');
        for (std::size_t position = 0; position < input.size();) {
            const std::size_t start = position;
            const auto byte = static_cast<unsigned char>(input[position++]);
            switch (byte) {
                case '"':
                    output_ += "\\\"";
                    break;
                case '\\':
                    output_ += "\\\\";
                    break;
                case '\b':
                    output_ += "\\b";
                    break;
                case '\f':
                    output_ += "\\f";
                    break;
                case '\n':
                    output_ += "\\n";
                    break;
                case '\r':
                    output_ += "\\r";
                    break;
                case '\t':
                    output_ += "\\t";
                    break;
                default:
                    if (byte < 0x20) {
                        output_ += "\\u00";
                        output_.push_back(hex[byte >> 4]);
                        output_.push_back(hex[byte & 0x0F]);
                    } else if (byte < 0x80) {
                        output_.push_back(static_cast<char>(byte));
                    } else {
                        std::size_t next = position;
                        if (!copy_utf8_sequence(input, start, next, output_)) {
                            return false;
                        }
                        position = next;
                    }
            }
        }
        output_.push_back('"');
        return true;
    }

    bool append_array(const value::array_type& input, std::size_t depth) {
        output_.push_back('[');
        if (input.empty()) {
            output_.push_back(']');
            return true;
        }
        if (pretty_) output_.push_back('\n');
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (pretty_) append_indent(depth + 1);
            if (!append_value(input[i], depth + 1)) return false;
            if (i + 1 != input.size()) output_.push_back(',');
            if (pretty_) output_.push_back('\n');
        }
        if (pretty_) append_indent(depth);
        output_.push_back(']');
        return true;
    }

    bool append_object(const value::object_type& input, std::size_t depth) {
        output_.push_back('{');
        if (input.empty()) {
            output_.push_back('}');
            return true;
        }
        if (pretty_) output_.push_back('\n');
        std::size_t index = 0;
        for (const auto& [key, child] : input) {
            if (pretty_) append_indent(depth + 1);
            if (!append_string(key)) return false;
            output_ += pretty_ ? ": " : ":";
            if (!append_value(child, depth + 1)) return false;
            if (++index != input.size()) output_.push_back(',');
            if (pretty_) output_.push_back('\n');
        }
        if (pretty_) append_indent(depth);
        output_.push_back('}');
        return true;
    }

    void append_indent(std::size_t depth) {
        output_.append(depth * 2, ' ');
    }

    bool pretty_;
    std::string output_;
};

}  // namespace detail

class simple_backend final : public backend {
public:
    result<value> parse(std::string_view text) override {
        try {
            return detail::parser(text).parse_document();
        } catch (...) {
            return result_err(std::errc::invalid_argument);
        }
    }

    result<std::string> stringify(const value& input, bool pretty) override {
        try {
            return detail::serializer(pretty).run(input);
        } catch (...) {
            return result_err(std::errc::invalid_argument);
        }
    }
};

}  // namespace utils::json

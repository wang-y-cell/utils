#pragma once

#include "reliability/result/expected.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace utils::json {

enum class kind {
    null,
    boolean,
    integer,
    number,
    string,
    array,
    object
};

class value {
public:
    using array_type = std::vector<value>;
    using object_type = std::map<std::string, value, std::less<>>;

    value() noexcept : storage_(nullptr) {}
    value(std::nullptr_t) noexcept : storage_(nullptr) {}
    value(bool input) : storage_(input) {}
    value(std::int64_t input) : storage_(input) {}
    value(double input) : storage_(input) {}
    value(std::string input) : storage_(std::move(input)) {}
    value(std::string_view input) : storage_(std::string(input)) {}
    value(const char* input) : storage_(std::string(input ? input : "")) {}
    value(array_type input) : storage_(std::move(input)) {}
    value(object_type input) : storage_(std::move(input)) {}

    [[nodiscard]] static value array() { return value(array_type{}); }
    [[nodiscard]] static value object() { return value(object_type{}); }

    [[nodiscard]] kind type() const noexcept {
        return static_cast<kind>(storage_.index());
    }

    [[nodiscard]] result<bool> as_bool() const {
        if (const auto* output = std::get_if<bool>(&storage_)) {
            return result_ok(*output);
        }
        return result_err(std::errc::invalid_argument);
    }

    [[nodiscard]] result<std::int64_t> as_int() const {
        if (const auto* output = std::get_if<std::int64_t>(&storage_)) {
            return result_ok(*output);
        }
        return result_err(std::errc::invalid_argument);
    }

    [[nodiscard]] result<double> as_double() const {
        if (const auto* output = std::get_if<double>(&storage_)) {
            return result_ok(*output);
        }
        if (const auto* integer = std::get_if<std::int64_t>(&storage_)) {
            return result_ok(static_cast<double>(*integer));
        }
        return result_err(std::errc::invalid_argument);
    }

    [[nodiscard]] result<std::string> as_string() const {
        if (const auto* output = std::get_if<std::string>(&storage_)) {
            return result_ok(*output);
        }
        return result_err(std::errc::invalid_argument);
    }

    [[nodiscard]] result<value> get(std::string_view dotted_path) const {
        if (dotted_path.empty()) {
            return result_err(std::errc::invalid_argument);
        }

        const value* current = this;
        std::size_t begin = 0;
        for (;;) {
            const std::size_t dot = dotted_path.find('.', begin);
            const std::size_t end =
                dot == std::string_view::npos ? dotted_path.size() : dot;
            const std::string_view key =
                dotted_path.substr(begin, end - begin);
            const auto* object_value = current->as_object();
            if (key.empty() || !object_value) {
                return result_err(std::errc::invalid_argument);
            }

            const auto found = object_value->find(key);
            if (found == object_value->end()) {
                return result_err(std::errc::no_such_file_or_directory);
            }
            current = &found->second;

            if (dot == std::string_view::npos) {
                return result_ok(*current);
            }
            begin = dot + 1;
        }
    }

    result<void> set(std::string_view dotted_path, value input) {
        if (dotted_path.empty()) {
            return result_err(std::errc::invalid_argument);
        }

        value* current = this;
        std::size_t begin = 0;
        for (;;) {
            const std::size_t dot = dotted_path.find('.', begin);
            const std::size_t end =
                dot == std::string_view::npos ? dotted_path.size() : dot;
            const std::string_view key =
                dotted_path.substr(begin, end - begin);
            auto* object_value = current->as_object();
            if (key.empty() || !object_value) {
                return result_err(std::errc::invalid_argument);
            }

            if (dot == std::string_view::npos) {
                object_value->insert_or_assign(std::string(key),
                                               std::move(input));
                return result_ok();
            }

            auto [found, inserted] =
                object_value->try_emplace(std::string(key), value::object());
            if (!inserted && found->second.type() != kind::object) {
                return result_err(std::errc::invalid_argument);
            }
            current = &found->second;
            begin = dot + 1;
        }
    }

    result<void> push_back(value input) {
        auto* array_value = as_array();
        if (!array_value) {
            return result_err(std::errc::invalid_argument);
        }
        array_value->push_back(std::move(input));
        return result_ok();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        if (const auto* array_value = as_array()) {
            return array_value->size();
        }
        if (const auto* object_value = as_object()) {
            return object_value->size();
        }
        return 0;
    }

    [[nodiscard]] const array_type* as_array() const noexcept {
        return std::get_if<array_type>(&storage_);
    }

    [[nodiscard]] array_type* as_array() noexcept {
        return std::get_if<array_type>(&storage_);
    }

    [[nodiscard]] const object_type* as_object() const noexcept {
        return std::get_if<object_type>(&storage_);
    }

    [[nodiscard]] object_type* as_object() noexcept {
        return std::get_if<object_type>(&storage_);
    }

private:
    using storage_type =
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                     array_type, object_type>;

    storage_type storage_;
};

}  // namespace utils::json

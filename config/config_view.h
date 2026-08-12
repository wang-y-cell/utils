#pragma once

/**
 * ConfigView — 配置只读视图（不绑具体解析库）
 *
 *   auto cfg = std::make_shared<utils::MapConfig>();
 *   cfg->set("port", 8080);
 *   auto port = cfg->get_int("port");
 *   auto db = cfg->section("db");  // 读 "db.host" 等
 *
 *   utils::EnvConfig env("MYAPP_");  // MYAPP_PORT
 */

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace utils {

class ConfigView {
public:
    virtual ~ConfigView() = default;

    [[nodiscard]] virtual std::optional<std::string> get_string(
        std::string_view key) const = 0;

    [[nodiscard]] virtual std::optional<std::int64_t> get_int(
        std::string_view key) const {
        auto s = get_string(key);
        if (!s) {
            return std::nullopt;
        }
        try {
            std::size_t idx = 0;
            auto v = std::stoll(*s, &idx, 10);
            if (idx != s->size()) {
                return std::nullopt;
            }
            return v;
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] virtual std::optional<bool> get_bool(
        std::string_view key) const {
        auto s = get_string(key);
        if (!s) {
            return std::nullopt;
        }
        if (*s == "1" || *s == "true" || *s == "TRUE" || *s == "yes" ||
            *s == "on") {
            return true;
        }
        if (*s == "0" || *s == "false" || *s == "FALSE" || *s == "no" ||
            *s == "off") {
            return false;
        }
        return std::nullopt;
    }

    [[nodiscard]] virtual std::optional<double> get_double(
        std::string_view key) const {
        auto s = get_string(key);
        if (!s) {
            return std::nullopt;
        }
        try {
            std::size_t idx = 0;
            auto v = std::stod(*s, &idx);
            if (idx != s->size()) {
                return std::nullopt;
            }
            return v;
        } catch (...) {
            return std::nullopt;
        }
    }

    /** 子节：后续 key 自动加 "name." 前缀 */
    [[nodiscard]] virtual std::shared_ptr<ConfigView> section(
        std::string_view name) const = 0;
};

namespace detail {

class PrefixedConfigView : public ConfigView {
public:
    PrefixedConfigView(std::shared_ptr<const ConfigView> base, std::string prefix)
        : base_(std::move(base)), prefix_(std::move(prefix)) {}

    [[nodiscard]] std::optional<std::string> get_string(
        std::string_view key) const override {
        return base_->get_string(prefix_ + std::string(key));
    }

    [[nodiscard]] std::shared_ptr<ConfigView> section(
        std::string_view name) const override {
        return std::make_shared<PrefixedConfigView>(
            base_, prefix_ + std::string(name) + ".");
    }

private:
    std::shared_ptr<const ConfigView> base_;
    std::string prefix_;
};

}  // namespace detail

/** 内存字典配置 */
class MapConfig : public ConfigView {
public:
    MapConfig() = default;

    void set(std::string key, std::string value) {
        data_[std::move(key)] = std::move(value);
    }

    void set(std::string key, const char* value) {
        data_[std::move(key)] = value ? std::string(value) : std::string{};
    }

    void set(std::string key, std::int64_t value) {
        data_[std::move(key)] = std::to_string(value);
    }

    void set(std::string key, double value) {
        data_[std::move(key)] = std::to_string(value);
    }

    /** 单独提供，避免 const char* 误匹配 bool */
    void set_bool(std::string key, bool value) {
        data_[std::move(key)] = value ? "true" : "false";
    }

    [[nodiscard]] std::optional<std::string> get_string(
        std::string_view key) const override {
        auto it = data_.find(std::string(key));
        if (it == data_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] std::shared_ptr<ConfigView> section(
        std::string_view name) const override {
        auto copy = std::make_shared<MapConfig>();
        copy->data_ = data_;
        return std::make_shared<detail::PrefixedConfigView>(
            std::move(copy), std::string(name) + ".");
    }

private:
    std::unordered_map<std::string, std::string> data_;
};

/** 环境变量：prefix + KEY，'.' → '_'，小写转大写 */
class EnvConfig : public ConfigView {
public:
    explicit EnvConfig(std::string prefix = {}) : prefix_(std::move(prefix)) {}

    [[nodiscard]] std::optional<std::string> get_string(
        std::string_view key) const override {
        const std::string env_key = prefix_ + to_env_key(key);
#if defined(_MSC_VER)
        char* buf = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&buf, &len, env_key.c_str()) != 0 || !buf) {
            return std::nullopt;
        }
        std::string val(buf);
        free(buf);
        return val;
#else
        if (const char* v = std::getenv(env_key.c_str())) {
            return std::string(v);
        }
        return std::nullopt;
#endif
    }

    [[nodiscard]] std::shared_ptr<ConfigView> section(
        std::string_view name) const override {
        return std::make_shared<EnvConfig>(prefix_ + to_env_key(name) + "_");
    }

private:
    static std::string to_env_key(std::string_view key) {
        std::string out;
        out.reserve(key.size());
        for (char c : key) {
            if (c == '.') {
                out.push_back('_');
            } else if (c >= 'a' && c <= 'z') {
                out.push_back(static_cast<char>(c - 'a' + 'A'));
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string prefix_;
};

}  // namespace utils

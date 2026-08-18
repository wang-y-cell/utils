/**
 * expected / result 使用教程（可运行）
 * 编译: cmake --build build --target demo_expected
 * 运行: ./build/demo_expected
 *
 * 要点:
 * - 业务失败: return result_err / unexpected（不要 throw）
 * - 取值: if (r) / *r / value_or；value() 无值才抛 bad_expected_access
 * - 链式: and_then / transform / or_else / transform_error
 */

#include "reliability/result/expected.h"

#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

using namespace utils;

result<int> parse_positive(std::string_view s) {
    if (s.empty()) return result_err(std::errc::invalid_argument);
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return result_err(std::errc::invalid_argument);
        n = n * 10 + (c - '0');
    }
    if (n <= 0) return result_err(std::errc::result_out_of_range);
    return result_ok(n);
}

result<void> ensure_ready(bool ready) {
    if (!ready) return result_err(std::errc::operation_not_permitted);
    return result_ok();
}

expected<std::string, std::string> load_name(bool ok_flag) {
    if (!ok_flag) return utils::unexpected(std::string{"name missing"});
    return expected<std::string, std::string>(std::in_place, "alice");
}

int main() {
    std::cout << "=== 检查与取值 ===\n";
    result<int> a = result_ok(42);
    result<int> b = result_err(std::errc::io_error);
    if (a) std::cout << "*a=" << *a << " value()=" << a.value() << '\n';
    if (!b) std::cout << "err=" << b.error().message() << '\n';
    std::cout << "value_or=" << b.value_or(0) << '\n';
    try {
        (void)b.value();
    } catch (const bad_expected_access& ex) {
        std::cout << "value() threw: " << ex.what() << '\n';
    }

    std::cout << "\n=== 调用 parse_positive ===\n";
    for (std::string_view s : {"42", "", "0", "x"}) {
        auto p = parse_positive(s);
        if (p) std::cout << "parse(" << s << ")=" << *p << '\n';
        else std::cout << "parse(" << s << ") err=" << p.error().message() << '\n';
    }
    std::cout << "ensure_ready(false)=" << static_cast<bool>(ensure_ready(false))
              << '\n';

    std::cout << "\n=== Monadic ===\n";
    auto d = parse_positive("21").and_then([](int n) -> result<int> {
        return result_ok(n * 2);
    });
    std::cout << "and_then=" << d.value_or(-1) << '\n';
    auto t = result_ok(41).transform([](int n) { return n + 1; });
    std::cout << "transform=" << *t << '\n';
    auto r = result<int>(result_err(std::errc::io_error)).or_else(
        [](const std::error_code&) -> result<int> { return result_ok(0); });
    std::cout << "or_else=" << *r << '\n';
    auto skipped = parse_positive("bad").transform([](int n) { return n; });
    std::cout << "short_circuit has_value=" << skipped.has_value() << '\n';
    expected<int, int> with_code(unexpect, 7);
    auto as_string = std::move(with_code).transform_error(
        [](int e) { return std::string{"code="} + std::to_string(e); });
    std::cout << "transform_error=" << as_string.error() << '\n';

    std::cout << "\n=== void / 自定义错误 ===\n";
    expected<void, int> okv;
    expected<void, int> bad = utils::unexpected(3);
    std::cout << "void_ok=" << okv.has_value() << " bad=" << bad.error() << '\n';
    auto name = load_name(false);
    if (!name) std::cout << "load_name err=" << name.error() << '\n';
    name = load_name(true);
    if (name) std::cout << "load_name=" << *name << '\n';

    std::cout << "\n=== 工厂 ===\n";
    auto x = ok(1);
    auto y = ok();
    result<int> z = result_err(std::errc::timed_out);
    std::cout << "ok(1)=" << *x << " void_ok=" << y.has_value()
              << " from_unexpect=" << z.has_value()
              << " result_ok=" << *result_ok(9)
              << " result_err=" << result_err(std::errc::broken_pipe).error().message()
              << '\n';

    std::cout << "\nexpected_demo: ok\n";
    return 0;
}

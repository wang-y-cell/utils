/**
 * expected / result 使用教程（可运行）
 * 编译: cmake --build build --target demo_expected
 * 运行: ./build/demo_expected
 *
 * 要点:
 * - 成功: return 值 / return {}（void）；失败: return err(...) / unexpected
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
    if (s.empty()) return err(std::errc::invalid_argument);
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return err(std::errc::invalid_argument);
        n = n * 10 + (c - '0');
    }
    if (n <= 0) return err(std::errc::result_out_of_range);
    return n;
}

result<void> ensure_ready(bool ready) {
    if (!ready) return err(std::errc::operation_not_permitted);
    return {};
}

result<std::string, std::string> load_name(bool ok_flag) {
    if (!ok_flag) return err(std::string{"name missing"});
    return std::string{"alice"};
}

int main() {
    std::cout << "=== 检查与取值 ===\n";
    result<int> a = 42;
    result<int> b = err(std::errc::io_error);
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
        return n * 2;
    });
    std::cout << "and_then=" << d.value_or(-1) << '\n';
    auto t = result<int>{41}.transform([](int n) { return n + 1; });
    std::cout << "transform=" << *t << '\n';
    auto r = result<int>(err(std::errc::io_error)).or_else(
        [](const std::error_code&) -> result<int> { return 0; });
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

    std::cout << "\n=== 构造 ===\n";
    result<int> x = 1;
    result<void> y{};
    result<int> z = err(std::errc::timed_out);
    std::cout << "success=" << *x << " void_success=" << y.has_value()
              << " from_unexpect=" << z.has_value()
              << " value=" << *result<int>{9}
              << " err=" << err(std::errc::broken_pipe).error().message()
              << '\n';

    std::cout << "\nexpected_demo: ok\n";
    return 0;
}

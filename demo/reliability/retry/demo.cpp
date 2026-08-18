/**
 * Retry / Backoff 用法演示
 * 编译: cmake --build build --target demo_retry
 *
 * 要点:
 * - retry_policy::fixed / exponential 只描述策略
 * - retry(op, policy [, should_retry] [, stop_token] [, deadline])
 * - op 必须返回 expected/result；业务失败用 result_err，不要靠异常做控制流
 * - 取消请直接用 std::stop_token / std::stop_source
 */

#include "reliability/retry/retry.h"
#include "reliability/time/deadline.h"

#include <chrono>
#include <iostream>
#include <stop_token>

using namespace utils;
using namespace std::chrono_literals;

int main() {
    std::cout << "=== 1) 指数退避直到成功 ===\n";
    int calls = 0;
    auto ok = retry(
        [&]() -> result<int> {
            ++calls;
            std::cout << "  attempt #" << calls << '\n';
            if (calls < 3) {
                return result_err(std::errc::connection_reset);
            }
            return result_ok(100);
        },
        retry_policy::exponential(5, 5ms));
    std::cout << "  value=" << ok.value_or(-1) << " calls=" << calls << '\n';

    std::cout << "\n=== 2) 谓词：部分错误不可重试 ===\n";
    calls = 0;
    auto fail = retry(
        [&]() -> result<int> {
            ++calls;
            return result_err(std::errc::invalid_argument);
        },
        retry_policy::fixed(5, 1ms),
        [](const std::error_code& ec) {
            // invalid_argument 不重试
            return ec != std::make_error_code(std::errc::invalid_argument);
        });
    std::cout << "  has_value=" << fail.has_value() << " calls=" << calls << '\n';

    std::cout << "\n=== 3) 与 std::stop_token 配合 ===\n";
    std::stop_source source;
    source.request_stop();
    auto canceled = retry(
        []() -> result<int> { return result_ok(1); },
        retry_policy::fixed(3, 1ms), source.get_token());
    std::cout << "  canceled err="
              << (canceled ? "none" : canceled.error().message()) << '\n';

    std::cout << "\ndemo_retry: ok\n";
    return 0;
}

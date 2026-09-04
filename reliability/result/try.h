#pragma once

/**
 * Try — result/expected 早退宏（成功继续，失败 return）
 *
 *   #include "reliability/result/try.h"
 *
 *   Try(save());           // 1 参：只检查（适合 result<void> 或只要传播）
 *   Try(x, parse());       // 2 参：解包成功值到 x
 *
 * 表达式里若有逗号，请再加一层括号，例如：
 *   Try((err(Code::X, "msg")));
 *   Try(x, (make_result(a, b)));
 *
 * 外层函数须返回与 expr 兼容的 result/expected（同类型可直接 return）。
 */

#include "reliability/result/expected.h"

#include <utility>

// 字符串拼接
#define UTILS_TRY_CAT(a, b) UTILS_TRY_CAT_I(a, b)
#define UTILS_TRY_CAT_I(a, b) a##b

#define UTILS_TRY_COUNT(_1, _2, N, ...) N //获得N
#define UTILS_TRY_COUNT_EXPAND(...) UTILS_TRY_COUNT(__VA_ARGS__)
#define UTILS_TRY_SELECT(...) UTILS_TRY_COUNT_EXPAND(__VA_ARGS__, 2, 1, 0)

// 用户输入   | UTILS_TRY_COUNT     |   _1   |   _2   | N
// save()   | save(), 2, 1, 0     | save() |    2   | 1
// n,save() | n, save(), 2, 1, 0  | n      | save() | 2

#define UTILS_TRY_1(expr)                                       \
    do {                                                        \
        auto&& _utils_try_r = (expr);                           \
        if (!_utils_try_r) {                                    \
            return ::utils::err(                                \
                std::move(_utils_try_r).error());               \
        }                                                       \
    } while (0)

#define UTILS_TRY_2(var, expr)                                  \
    auto&& UTILS_TRY_CAT(var, _utils_try_r) = (expr);           \
    if (!UTILS_TRY_CAT(var, _utils_try_r)) {                    \
        return ::utils::err(                                    \
            std::move(UTILS_TRY_CAT(var, _utils_try_r)).error()); \
    }                                                           \
    auto&& var = *std::move(UTILS_TRY_CAT(var, _utils_try_r))

#define UTILS_TRY(...) \
    UTILS_TRY_CAT(UTILS_TRY_, UTILS_TRY_SELECT(__VA_ARGS__))(__VA_ARGS__)
    //字符串拼接 UTILS_TRY_ + 1 或 2

/** @brief 对外短名：Try(expr) / Try(var, expr) */
#define Try(...) UTILS_TRY(__VA_ARGS__)

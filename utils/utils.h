#pragma once

/**
 * utils 伞头 — 胶水层一键引入（不含 thread_pool / signal_and_slots）
 *
 *   #include "utils/utils.h"
 */

#include "result/expected.h"
#include "scope_guard/scope_guard.h"
#include "functional/function_ref.h"
#include "functional/any_invocable.h"
#include "span/span_utils.h"
#include "executor/executor.h"
// thread_pool / EventLoop 适配：#include "executor/adapters.h"

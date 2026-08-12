#pragma once

/**
 * utils 伞头 — 胶水层一键引入
 *
 *   #include "utils/utils.h"
 *
 * thread_pool / EventLoop 适配另含：#include "executor/adapters.h"
 * 信号槽：#include "signal_and_slots/signal_and_slots.h"
 */

#include "result/expected.h"
#include "scope_guard/scope_guard.h"
#include "functional/function_ref.h"
#include "functional/any_invocable.h"
#include "span/span_utils.h"
#include "executor/executor.h"
#include "cancel/cancellation.h"
#include "time/deadline.h"
#include "retry/retry.h"
#include "channel/channel.h"
#include "config/config_view.h"
#include "log/log.h"

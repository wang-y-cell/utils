#pragma once

/**
 * utils 伞头 — 胶水层一键引入
 *
 *   #include "utils/utils.h"
 *
 * 目录分层：
 *   adapter/   — 门面（config / log / executor / sql）
 *   component/ — 功能组件（expected、thread_pool、channel …）
 *
 * thread_pool / event_loop 适配另含：#include "adapter/executor/adapters.h"
 * 信号槽：#include "component/signal_and_slots/signal_and_slots.h"
 *
 * 规划清单见 docs/adapter-plan.md
 * 网络请直接使用 Boost.Asio / Beast，本库不做网络门面。
 */

#include "component/result/expected.h"
#include "component/scope_guard/scope_guard.h"
#include "component/functional/function_ref.h"
#include "component/functional/any_invocable.h"
#include "adapter/executor/executor.h"
#include "component/time/deadline.h"
#include "component/retry/retry.h"
#include "component/channel/channel.h"
#include "adapter/config/config_view.h"
#include "adapter/log/log.h"
#include "adapter/sql/sql.h"

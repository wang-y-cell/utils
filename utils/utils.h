#pragma once

/**
 * utils 伞头 — 三主题一键引入
 *
 *   #include "utils/utils.h"
 *
 * 主题目录：
 *   concurrency/  — 并发与事件（signal、thread_pool、channel、executor）
 *   reliability/  — 可靠性与错误（result、retry、deadline）
 *   memory/       — 内存管理（memory_pool、object_pool）
 *
 * 按需另含：
 *   #include "concurrency/concurrency.h"          // 仅并发主题
 *   #include "concurrency/executor/adapters.h"    // thread_pool / event_loop 适配
 *   #include "concurrency/signal_and_slots/signal_and_slots.h"
 *
 * 文档：docs/README.md
 */

#include "concurrency/concurrency.h"
#include "memory/memory.h"
#include "reliability/reliability.h"

#pragma once

/**
 * 主题 1：并发与事件
 *
 *   #include "concurrency/concurrency.h"
 *
 * 信号槽 / thread_pool 体量较大，也可按需单独 include：
 *   #include "concurrency/signal_and_slots/signal_and_slots.h"
 *   #include "concurrency/thread_pool/thread_pool.h"
 *   #include "concurrency/executor/adapters.h"
 */

#include "concurrency/channel/channel.h"
#include "concurrency/executor/executor.h"

/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_SCHEDULER_H_
#define OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_SCHEDULER_H_

#include <pthread.h>

#include "lib/lock/ob_mutex.h"
#include "lib/thread/ob_queue_thread.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "lib/oblog/ob_log_module.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace sql
{

// A single pending fill of an AI column.
//
// The INSERT/UPDATE executor creates one of these for every row it writes to
// a table that owns an ASYNCHRONOUS generated column.  A background worker
// later evaluates the AI expression and writes the produced value back into
// the row, so the DML statement itself never waits on the model.
struct ObAIColumnTask
{
  ObAIColumnTask()
    : table_id_(common::OB_INVALID_ID),
      row_scn_(0),
      column_id_(common::OB_INVALID_ID),
      retry_count_(0),
      enqueue_ts_(0)
  {}

  void reset()
  {
    table_id_ = common::OB_INVALID_ID;
    row_scn_ = 0;
    column_id_ = common::OB_INVALID_ID;
    retry_count_ = 0;
    enqueue_ts_ = 0;
  }

  TO_STRING_KV(K_(table_id), K_(row_scn), K_(column_id), K_(retry_count));

  uint64_t table_id_;      // table that owns the AI column
  int64_t row_scn_;        // row version the value is produced for
  uint64_t column_id_;     // the AI column to fill
  int64_t retry_count_;    // number of attempts so far
  int64_t enqueue_ts_;     // enqueue time, microseconds
};

// Bounded FIFO of pending AI column fills.
class ObAIColumnTaskQueue
{
public:
  ObAIColumnTaskQueue();
  ~ObAIColumnTaskQueue() {}

  // Appends one task; returns OB_SIZE_OVERFLOW when the backlog is too deep.
  int push(const ObAIColumnTask &task);
  // Waits up to timeout_us for a task.  Returns OB_TIMEOUT when idle.
  int pop(ObAIColumnTask &task, const int64_t timeout_us);
  int64_t size() const;
  void signal();

private:
  static const int64_t MAX_BACKLOG = 4096;
  mutable lib::ObMutex mutex_;
  common::ObCond cond_;
  common::ObSEArray<ObAIColumnTask, 64> tasks_;
};

// Drains the queue on a dedicated thread.
//
// The thread is created lazily by ObAIColumnScheduler::start() and stopped by
// stop().  Every task is evaluated outside of any user session, so a slow AI
// call can never block a client statement.
class ObAIColumnScheduler
{
public:
  ObAIColumnScheduler();
  ~ObAIColumnScheduler();

  int start();
  void stop();

  // Enqueues a fill request; safe to call from any SQL worker thread.
  static int enqueue(const ObAIColumnTask &task);

  int64_t backlog() const { return queue_.size(); }

private:
  static void *run(void *arg);
  void worker_loop();
  int process_task(ObAIColumnTask &task);

  ObAIColumnTaskQueue queue_;
  pthread_t thread_;
  volatile bool started_;
  volatile bool stopped_;
};

// Process wide instance, installed by ObServer during start up.  All accessors
// tolerate a NULL scheduler so statements keep working when the background
// worker is disabled.
ObAIColumnScheduler *get_ai_column_scheduler();
void set_ai_column_scheduler(ObAIColumnScheduler *scheduler);

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_SCHEDULER_H_

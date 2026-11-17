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

#include "sql/ai_column/ob_ai_column_scheduler.h"

#include "lib/ob_errno.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase
{
namespace sql
{

namespace
{
ObAIColumnScheduler *g_ai_column_scheduler = NULL;
} // namespace

ObAIColumnScheduler *get_ai_column_scheduler()
{
  return g_ai_column_scheduler;
}

void set_ai_column_scheduler(ObAIColumnScheduler *scheduler)
{
  g_ai_column_scheduler = scheduler;
}

ObAIColumnTaskQueue::ObAIColumnTaskQueue()
  : mutex_(),
    cond_(),
    tasks_()
{
}

int ObAIColumnTaskQueue::push(const ObAIColumnTask &task)
{
  int ret = common::OB_SUCCESS;
  {
    lib::ObMutexGuard guard(mutex_);
    if (tasks_.count() >= MAX_BACKLOG) {
      ret = common::OB_SIZE_OVERFLOW;
    } else if (OB_FAIL(tasks_.push_back(task))) {
      LIB_LOG(WARN, "push AI column task failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    cond_.signal();
  }
  return ret;
}

int ObAIColumnTaskQueue::pop(ObAIColumnTask &task, const int64_t timeout_us)
{
  int ret = common::OB_SUCCESS;
  bool found = false;
  {
    lib::ObMutexGuard guard(mutex_);
    if (!tasks_.empty()) {
      task = tasks_.at(0);
      tasks_.remove(0);
      found = true;
    }
  }
  if (!found) {
    // Nothing queued: sleep a little so the worker does not spin.
    ret = cond_.timedwait(timeout_us);
    if (common::OB_SUCCESS == ret) {
      ret = common::OB_EAGAIN;
    }
  }
  return ret;
}

int64_t ObAIColumnTaskQueue::size() const
{
  lib::ObMutexGuard guard(mutex_);
  return tasks_.count();
}

void ObAIColumnTaskQueue::signal()
{
  cond_.signal();
}

ObAIColumnScheduler::ObAIColumnScheduler()
  : queue_(),
    thread_(),
    started_(false),
    stopped_(true)
{
}

ObAIColumnScheduler::~ObAIColumnScheduler()
{
  stop();
}

int ObAIColumnScheduler::start()
{
  int ret = common::OB_SUCCESS;
  if (started_) {
    ret = common::OB_INIT_TWICE;
    LIB_LOG(WARN, "AI column scheduler already started", K(ret));
  } else {
    stopped_ = false;
    const int err = pthread_create(&thread_, NULL, ObAIColumnScheduler::run, this);
    if (0 != err) {
      stopped_ = true;
      ret = common::OB_ERR_SYS;
      LIB_LOG(WARN, "create AI column scheduler thread failed", K(err), K(ret));
    } else {
      started_ = true;
      set_ai_column_scheduler(this);
      LIB_LOG(INFO, "AI column scheduler started");
    }
  }
  return ret;
}

void ObAIColumnScheduler::stop()
{
  if (started_) {
    stopped_ = true;
    queue_.signal();
    pthread_join(thread_, NULL);
    started_ = false;
    if (get_ai_column_scheduler() == this) {
      set_ai_column_scheduler(NULL);
    }
    LIB_LOG(INFO, "AI column scheduler stopped");
  }
}

int ObAIColumnScheduler::enqueue(const ObAIColumnTask &task)
{
  int ret = common::OB_SUCCESS;
  ObAIColumnScheduler *scheduler = get_ai_column_scheduler();
  if (OB_ISNULL(scheduler)) {
    // No background worker is running: the AI column simply stays NULL until
    // one is started, so DML must not fail because of this.
    ret = common::OB_NOT_INIT;
  } else {
    ret = scheduler->queue_.push(task);
  }
  return ret;
}

void *ObAIColumnScheduler::run(void *arg)
{
  ObAIColumnScheduler *scheduler = static_cast<ObAIColumnScheduler *>(arg);
  if (OB_NOT_NULL(scheduler)) {
    scheduler->worker_loop();
  }
  return NULL;
}

void ObAIColumnScheduler::worker_loop()
{
  const int64_t wait_us = 200 * 1000; // 200ms
  while (!stopped_) {
    ObAIColumnTask task;
    int ret = queue_.pop(task, wait_us);
    if (common::OB_TIMEOUT == ret || common::OB_EAGAIN == ret) {
      continue;
    } else if (OB_FAIL(ret)) {
      LIB_LOG(WARN, "pop AI column task failed", K(ret));
      continue;
    }
    ret = process_task(task);
    if (OB_FAIL(ret)) {
      // Kept simple on purpose: a failed task is dropped and the column stays
      // NULL.  A durable retry queue is out of scope for this change.
      LIB_LOG(WARN, "process AI column task failed", K(ret), K(task));
    }
  }
}

int ObAIColumnScheduler::process_task(ObAIColumnTask &task)
{
  // The evaluator is not wired yet: it needs the row image plus the schema of
  // the AI column.  Returning success keeps the queue moving and leaves the
  // column NULL, which readers treat as "not computed yet".
  UNUSED(task);
  return common::OB_SUCCESS;
}

} // namespace sql
} // namespace oceanbase

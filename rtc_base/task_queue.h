/*
 *  Copyright 2016 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef RTC_BASE_TASK_QUEUE_H_
#define RTC_BASE_TASK_QUEUE_H_

#include <stdint.h>
#include <memory>
#include <type_traits>
#include <utility>

#include "absl/memory/memory.h"
#include "api/task_queue/queued_task.h"
#include "api/task_queue/task_queue_priority.h"
#include "rtc_base/constructor_magic.h"
#include "rtc_base/scoped_ref_ptr.h"
#include "rtc_base/system/rtc_export.h"
#include "rtc_base/thread_annotations.h"

namespace rtc {

// TODO(danilchap): Remove the alias when all of webrtc is updated to use
// webrtc::QueuedTask directly.
using ::webrtc::QueuedTask;

// Simple implementation of QueuedTask for use with rtc::Bind and lambdas.
template <class Closure>
class ClosureTask : public QueuedTask {
 public:
  explicit ClosureTask(Closure&& closure)
      : closure_(std::forward<Closure>(closure)) {}

 private:
  bool Run() override {
    closure_();
    return true;
  }

  typename std::remove_const<
      typename std::remove_reference<Closure>::type>::type closure_;
};

// Extends ClosureTask to also allow specifying cleanup code.
// This is useful when using lambdas if guaranteeing cleanup, even if a task
// was dropped (queue is too full), is required.
template <class Closure, class Cleanup>
class ClosureTaskWithCleanup : public ClosureTask<Closure> {
 public:
  ClosureTaskWithCleanup(Closure&& closure, Cleanup&& cleanup)
      : ClosureTask<Closure>(std::forward<Closure>(closure)),
        cleanup_(std::forward<Cleanup>(cleanup)) {}
  ~ClosureTaskWithCleanup() { cleanup_(); }

 private:
  typename std::remove_const<
      typename std::remove_reference<Cleanup>::type>::type cleanup_;
};

// Convenience function to construct closures that can be passed directly
// to methods that support std::unique_ptr<QueuedTask> but not template
// based parameters.
template <class Closure>
static std::unique_ptr<QueuedTask> NewClosure(Closure&& closure) {
  return absl::make_unique<ClosureTask<Closure>>(
      std::forward<Closure>(closure));
}

template <class Closure, class Cleanup>
static std::unique_ptr<QueuedTask> NewClosure(Closure&& closure,
                                              Cleanup&& cleanup) {
  return absl::make_unique<ClosureTaskWithCleanup<Closure, Cleanup>>(
      std::forward<Closure>(closure), std::forward<Cleanup>(cleanup));
}

// Implements a task queue that asynchronously executes tasks in a way that
// guarantees that they're executed in FIFO order and that tasks never overlap.
// Tasks may always execute on the same worker thread and they may not.
// To DCHECK that tasks are executing on a known task queue, use IsCurrent().
//
// Here are some usage examples:
//
//   1) Asynchronously running a lambda:
//
//     class MyClass {
//       ...
//       TaskQueue queue_("MyQueue");
//     };
//
//     void MyClass::StartWork() {
//       queue_.PostTask([]() { Work(); });
//     ...
//
//   2) Doing work asynchronously on a worker queue and providing a notification
//      callback on the current queue, when the work has been done:
//
//     void MyClass::StartWorkAndLetMeKnowWhenDone(
//         std::unique_ptr<QueuedTask> callback) {
//       DCHECK(TaskQueue::Current()) << "Need to be running on a queue";
//       queue_.PostTaskAndReply([]() { Work(); }, std::move(callback));
//     }
//     ...
//     my_class->StartWorkAndLetMeKnowWhenDone(
//         NewClosure([]() { RTC_LOG(INFO) << "The work is done!";}));
//
//   3) Posting a custom task on a timer.  The task posts itself again after
//      every running:
//
//     class TimerTask : public QueuedTask {
//      public:
//       TimerTask() {}
//      private:
//       bool Run() override {
//         ++count_;
//         TaskQueue::Current()->PostDelayedTask(
//             std::unique_ptr<QueuedTask>(this), 1000);
//         // Ownership has been transferred to the next occurance,
//         // so return false to prevent from being deleted now.
//         return false;
//       }
//       int count_ = 0;
//     };
//     ...
//     queue_.PostDelayedTask(
//         std::unique_ptr<QueuedTask>(new TimerTask()), 1000);
//
// For more examples, see task_queue_unittests.cc.
//
// A note on destruction:
//
// When a TaskQueue is deleted, pending tasks will not be executed but they will
// be deleted.  The deletion of tasks may happen asynchronously after the
// TaskQueue itself has been deleted or it may happen synchronously while the
// TaskQueue instance is being deleted.  This may vary from one OS to the next
// so assumptions about lifetimes of pending tasks should not be made.
class RTC_LOCKABLE RTC_EXPORT TaskQueue {
 public:
  // TaskQueue priority levels. On some platforms these will map to thread
  // priorities, on others such as Mac and iOS, GCD queue priorities.
  using Priority = ::webrtc::TaskQueuePriority;
  class Impl;

  explicit TaskQueue(const char* queue_name,
                     Priority priority = Priority::NORMAL);
  ~TaskQueue();

  static TaskQueue* Current();

  // Used for DCHECKing the current queue.
  bool IsCurrent() const;

  // TODO(tommi): For better debuggability, implement RTC_FROM_HERE.

  // Ownership of the task is passed to PostTask.
  void PostTask(std::unique_ptr<QueuedTask> task);

  // Schedules a task to execute a specified number of milliseconds from when
  // the call is made. The precision should be considered as "best effort"
  // and in some cases, such as on Windows when all high precision timers have
  // been used up, can be off by as much as 15 millseconds (although 8 would be
  // more likely). This can be mitigated by limiting the use of delayed tasks.
  void PostDelayedTask(std::unique_ptr<QueuedTask> task, uint32_t milliseconds);

  // std::enable_if is used here to make sure that calls to PostTask() with
  // std::unique_ptr<SomeClassDerivedFromQueuedTask> would not end up being
  // caught by this template.
  template <class Closure,
            typename std::enable_if<!std::is_convertible<
                Closure,
                std::unique_ptr<QueuedTask>>::value>::type* = nullptr>
  void PostTask(Closure&& closure) {
    PostTask(NewClosure(std::forward<Closure>(closure)));
  }

  // See documentation above for performance expectations.
  template <class Closure,
            typename std::enable_if<!std::is_convertible<
                Closure,
                std::unique_ptr<QueuedTask>>::value>::type* = nullptr>
  void PostDelayedTask(Closure&& closure, uint32_t milliseconds) {
    PostDelayedTask(NewClosure(std::forward<Closure>(closure)), milliseconds);
  }

 private:
  // TODO(danilchap): Remove when external implementaions of TaskQueue remove
  // these two functions.
  void PostTaskAndReply(std::unique_ptr<QueuedTask> task,
                        std::unique_ptr<QueuedTask> reply,
                        TaskQueue* reply_queue);
  void PostTaskAndReply(std::unique_ptr<QueuedTask> task,
                        std::unique_ptr<QueuedTask> reply);

  const scoped_refptr<Impl> impl_;

  RTC_DISALLOW_COPY_AND_ASSIGN(TaskQueue);
};

}  // namespace rtc

#endif  // RTC_BASE_TASK_QUEUE_H_

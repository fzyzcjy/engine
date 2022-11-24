// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/vsync_waiter.h"
#include <sys/time.h>
#include <tonic/typed_data/dart_byte_data.h>

#include "flow/frame_timings.h"
#include "flutter/fml/task_runner.h"
#include "flutter/fml/trace_event.h"
#include "fml/logging.h"
#include "fml/message_loop_task_queues.h"
#include "fml/task_queue_id.h"
#include "fml/time/time_point.h"

namespace flutter {

static constexpr const char* kVsyncFlowName = "VsyncFlow";

static constexpr const char* kVsyncTraceName = "VsyncProcessCallback";

fml::TimePoint LastVsyncInfo::GetVsyncStartTime() const {
  std::scoped_lock state_lock(mutex_);
  return vsync_start_;
}

fml::TimePoint LastVsyncInfo::GetVsyncTargetTime() const {
  std::scoped_lock state_lock(mutex_);
  return vsync_target_;
}

int64_t LastVsyncInfo::GetDiffDateTimeTimePoint() const {
  std::scoped_lock state_lock(mutex_);
  return diff_date_time_time_point_;
}

// ref: Dart DateTime.now() implementation
// https://github.com/fzyzcjy/yplusplus/issues/5834#issuecomment-1257329034
int64_t DartCompatibleGetCurrentTimeMicros() {
  // gettimeofday has microsecond resolution.
  struct timeval tv;
  if (gettimeofday(&tv, NULL) < 0) {
    return 0;
  }
  return (static_cast<int64_t>(tv.tv_sec) * 1000000) + tv.tv_usec;
}

void LastVsyncInfo::RecordVsync(fml::TimePoint vsync_start,
                                fml::TimePoint vsync_target) {
  FML_DLOG(INFO) << "hi LastVsyncInfo::RecordVsync"
                 << " this_thread_id=" << pthread_self() << " vsync_start="
                 << (vsync_start - fml::TimePoint()).ToMicroseconds()
                 << " vsync_target="
                 << (vsync_target - fml::TimePoint()).ToMicroseconds();

  int64_t curr_datetime = DartCompatibleGetCurrentTimeMicros();
  fml::TimePoint curr_time = fml::TimePoint::Now();
  int64_t diff_date_time_time_point =
      curr_datetime - (curr_time - fml::TimePoint()).ToMicroseconds();

  std::scoped_lock state_lock(mutex_);
  vsync_start_ = vsync_start;
  vsync_target_ = vsync_target;
  diff_date_time_time_point_ = diff_date_time_time_point;
}

LastVsyncInfo& LastVsyncInfo::Instance() {
  static LastVsyncInfo instance;
  return instance;
}

Dart_Handle LastVsyncInfo::ReadToDart() {
  // https://github.com/fzyzcjy/flutter_smooth/issues/38#issuecomment-1262271851
  FML_LOG(ERROR) << "LastVsyncInfo::ReadToDart is temporarily disabled!";
  abort();

  LastVsyncInfo& instance = Instance();
  auto vsync_target_time = instance.GetVsyncTargetTime();
  auto diff_date_time_time_point = instance.GetDiffDateTimeTimePoint();

  // ref OnAnimatorBeginFrame -> ... -> begin_frame_, uses GetVsyncTargetTime
  // ref PlatformConfiguration::BeginFrame
  int64_t vsync_target_time_us =
      (vsync_target_time - fml::TimePoint()).ToMicroseconds();

  std::vector<int64_t> data{vsync_target_time_us, diff_date_time_time_point};
  return tonic::DartConverter<std::vector<int64_t>>::ToDart(data);
}

VsyncWaiter::VsyncWaiter(const TaskRunners& task_runners)
    : task_runners_(task_runners) {}

VsyncWaiter::~VsyncWaiter() = default;

// Public method invoked by the animator.
void VsyncWaiter::AsyncWaitForVsync(const Callback& callback) {
  if (!callback) {
    return;
  }

  TRACE_EVENT0("flutter", "AsyncWaitForVsync");

  {
    std::scoped_lock lock(callback_mutex_);
    if (callback_) {
      // The animator may request a frame more than once within a frame
      // interval. Multiple calls to request frame must result in a single
      // callback per frame interval.
      TRACE_EVENT_INSTANT0("flutter", "MultipleCallsToVsyncInFrameInterval");
      return;
    }
    callback_ = callback;
    if (!secondary_callbacks_.empty()) {
      // Return directly as `AwaitVSync` is already called by
      // `ScheduleSecondaryCallback`.
      return;
    }
  }
  AwaitVSync();
}

void VsyncWaiter::ScheduleSecondaryCallback(uintptr_t id,
                                            const fml::closure& callback,
                                            bool sanity_check_thread) {
  // NOTE HACK #5831
  FML_DCHECK(!sanity_check_thread ||
             task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (!callback) {
    return;
  }

  TRACE_EVENT0("flutter", "ScheduleSecondaryCallback");

  {
    std::scoped_lock lock(callback_mutex_);
    bool secondary_callbacks_originally_empty = secondary_callbacks_.empty();
    auto [_, inserted] = secondary_callbacks_.emplace(id, callback);
    if (!inserted) {
      // Multiple schedules must result in a single callback per frame interval.
      TRACE_EVENT_INSTANT0("flutter",
                           "MultipleCallsToSecondaryVsyncInFrameInterval");
      return;
    }
    if (callback_) {
      // Return directly as `AwaitVSync` is already called by
      // `AsyncWaitForVsync`.
      return;
    }
    if (!secondary_callbacks_originally_empty) {
      // Return directly as `AwaitVSync` is already called by
      // `ScheduleSecondaryCallback`.
      return;
    }
  }
  AwaitVSyncForSecondaryCallback();
}

// void TracePseudoVsync(fml::TimePoint start, fml::TimePoint end) {
//   // not working
//   // https://github.com/fzyzcjy/yplusplus/issues/6049#issuecomment-1270892321
//   //  fml::tracing::TraceEventAsyncComplete("flutter", "VSYNC",
//   //  frame_start_time,
//   //                                        frame_target_time);
//
//   // ref [TraceEvent0]
//   fml::tracing::TraceTimelineEvent("flutter", "VSYNC",
//                                    start.ToEpochDelta().ToMicroseconds(), 0,
//                                    Dart_Timeline_Event_Begin, {}, {});
//   // ref [TraceEventEnd]
//   fml::tracing::TraceTimelineEvent("flutter", "VSYNC",
//                                    end.ToEpochDelta().ToMicroseconds(), 0,
//                                    Dart_Timeline_Event_End, {}, {});
// }

void VsyncWaiter::FireCallback(fml::TimePoint frame_start_time,
                               fml::TimePoint frame_target_time,
                               bool pause_secondary_tasks) {
  //  FML_DLOG(INFO)
  //      << "hi VsyncWaiter::FireCallback start"
  //      << " this_thread_id=" << pthread_self() << " is-on-platform-thread="
  //      << task_runners_.GetPlatformTaskRunner()->RunsTasksOnCurrentThread()
  //      << " is-on-ui-thread="
  //      << task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread()
  //      << " is-on-io-thread="
  //      << task_runners_.GetIOTaskRunner()->RunsTasksOnCurrentThread()
  //      << " is-on-raster-thread="
  //      << task_runners_.GetRasterTaskRunner()->RunsTasksOnCurrentThread();
  FML_DCHECK(fml::TimePoint::Now() >= frame_start_time);

  Callback callback;
  std::vector<fml::closure> secondary_callbacks;

  {
    std::scoped_lock lock(callback_mutex_);
    callback = std::move(callback_);
    for (auto& pair : secondary_callbacks_) {
      secondary_callbacks.push_back(std::move(pair.second));
    }
    secondary_callbacks_.clear();
  }

  if (!callback && secondary_callbacks.empty()) {
    // This means that the vsync waiter implementation fired a callback for a
    // request we did not make. This is a paranoid check but we still want to
    // make sure we catch misbehaving vsync implementations.
    TRACE_EVENT_INSTANT0("flutter", "MismatchedFrameCallback");
    return;
  }

  // NOTE must be after "callback empty then return", b/c a flutter bug
  // #5835
  LastVsyncInfo::Instance().RecordVsync(frame_start_time, frame_target_time);

  // hack: schedule immediately to ensure [LastVsyncInfo] is updated every 16ms
  // in real implementation, will instead have real start/pause mechanism
  // instead of such blindly refresh
  // #5831
  // NOTE HACK about threads:
  // * With current hack, FireCallback is in platform thread
  // * Current AwaitVsync (android + not-ndk) can be called in PlatformThread
  // but need hack for other platforms as well
  //  FML_DLOG(INFO) << "hi VsyncWaiter::FireCallback extra call AwaitVsync to "
  //                    "ensure every frame we see info";
  // NOTE must be *after* checking empty and early return
  //      for that, see #5835
  task_runners_.GetPlatformTaskRunner()->PostDelayedTask(
      [this] {
        ScheduleSecondaryCallback(
            reinterpret_cast<uintptr_t>(&LastVsyncInfo::Instance()), [] {},
            // NOTE do NOT sanity check thread, since closure is empty and we
            // only want to trigger scheduling
            false);
      },
      // NOTE add delay - without this delay, have seen FireCallback multi
      // times for roughly one vsync within a few milliseconds
      // #6197
      fml::TimeDelta::FromMilliseconds(3));

  // ref [TraceEvent0], [TraceEventEnd]
  fml::tracing::TraceTimelineEvent(
      "flutter", "VSYNC", frame_start_time.ToEpochDelta().ToMicroseconds(), 0,
      Dart_Timeline_Event_Begin, {"frame_start_time", "frame_target_time"},
      {std::to_string(frame_start_time.ToEpochDelta().ToMicroseconds()),
       std::to_string(frame_target_time.ToEpochDelta().ToMicroseconds())});
  fml::tracing::TraceTimelineEvent(
      "flutter", "VSYNC", frame_target_time.ToEpochDelta().ToMicroseconds(), 0,
      Dart_Timeline_Event_End, {}, {});

  // for debug #5988
  // use the name "VSYNC" since #6049
  // commented out in #6197
  //  if (last_timeline_report_vsync_target_time_.has_value()) {
  //    fml::TimeDelta rough_frame_duration = frame_target_time -
  //    frame_start_time; for (fml::TimePoint t = frame_target_time -
  //    rough_frame_duration;
  //         t > last_timeline_report_vsync_target_time_.value() +
  //                 fml::TimeDelta::FromMilliseconds(1);
  //         t = t - rough_frame_duration) {
  //      TracePseudoVsync(t - rough_frame_duration,
  //                       // -1us to avoid multi events be treated as one (?)
  //                       t - fml::TimeDelta::FromMicroseconds(1));
  //    }
  //  }
  //  last_timeline_report_vsync_target_time_ = frame_target_time;

  if (callback) {
    auto flow_identifier = fml::tracing::TraceNonce();
    if (pause_secondary_tasks) {
      PauseDartMicroTasks();
    }

    // The base trace ensures that flows have a root to begin from if one does
    // not exist. The trace viewer will ignore traces that have no base event
    // trace. While all our message loops insert a base trace trace
    // (MessageLoop::RunExpiredTasks), embedders may not.
    TRACE_EVENT0("flutter", "VsyncFireCallback");

    TRACE_FLOW_BEGIN("flutter", kVsyncFlowName, flow_identifier);

    fml::TaskQueueId ui_task_queue_id =
        task_runners_.GetUITaskRunner()->GetTaskQueueId();

    task_runners_.GetUITaskRunner()->PostTask(
        [ui_task_queue_id, callback, flow_identifier, frame_start_time,
         frame_target_time, pause_secondary_tasks]() {
          //          FML_DLOG(INFO) << "hi VsyncWaiter::FireCallback inside
          //          UITaskRunner "
          //                            "PostTask callback start";
          FML_TRACE_EVENT("flutter", kVsyncTraceName, "StartTime",
                          frame_start_time, "TargetTime", frame_target_time);
          std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder =
              std::make_unique<FrameTimingsRecorder>();
          frame_timings_recorder->RecordVsync(frame_start_time,
                                              frame_target_time);
          callback(std::move(frame_timings_recorder));
          TRACE_FLOW_END("flutter", kVsyncFlowName, flow_identifier);
          if (pause_secondary_tasks) {
            ResumeDartMicroTasks(ui_task_queue_id);
          }
          //          FML_DLOG(INFO) << "hi VsyncWaiter::FireCallback inside
          //          UITaskRunner "
          //                            "PostTask callback end";
        });
  }

  for (auto& secondary_callback : secondary_callbacks) {
    task_runners_.GetUITaskRunner()->PostTask(secondary_callback);
  }
  //  FML_DLOG(INFO) << "hi VsyncWaiter::FireCallback end";
}

void VsyncWaiter::PauseDartMicroTasks() {
  auto ui_task_queue_id = task_runners_.GetUITaskRunner()->GetTaskQueueId();
  auto task_queues = fml::MessageLoopTaskQueues::GetInstance();
  task_queues->PauseSecondarySource(ui_task_queue_id);
}

void VsyncWaiter::ResumeDartMicroTasks(fml::TaskQueueId ui_task_queue_id) {
  auto task_queues = fml::MessageLoopTaskQueues::GetInstance();
  task_queues->ResumeSecondarySource(ui_task_queue_id);
}

}  // namespace flutter

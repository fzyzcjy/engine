// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/animator.h"

#include "flutter/flow/frame_timings.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/fml/trace_event.h"
#include "third_party/dart/runtime/include/dart_tools_api.h"

namespace flutter {

namespace {

// Wait 51 milliseconds (which is 1 more milliseconds than 3 frames at 60hz)
// before notifying the engine that we are idle.  See comments in |BeginFrame|
// for further discussion on why this is necessary.
constexpr fml::TimeDelta kNotifyIdleTaskWaitTime =
    fml::TimeDelta::FromMilliseconds(51);

}  // namespace

Animator::Animator(Delegate& delegate,
                   const TaskRunners& task_runners,
                   std::unique_ptr<VsyncWaiter> waiter)
    : delegate_(delegate),
      task_runners_(task_runners),
      waiter_(std::move(waiter)),
#if SHELL_ENABLE_METAL
      layer_tree_pipeline_(std::make_shared<LayerTreePipeline>(2)),
#else   // SHELL_ENABLE_METAL
      // TODO(dnfield): We should remove this logic and set the pipeline depth
      // back to 2 in this case. See
      // https://github.com/flutter/engine/pull/9132 for discussion.
      layer_tree_pipeline_(std::make_shared<LayerTreePipeline>(
          task_runners.GetPlatformTaskRunner() ==
                  task_runners.GetRasterTaskRunner()
              ? 1
              : 2)),
#endif  // SHELL_ENABLE_METAL
        //      pending_frame_semaphore_(1),
      weak_factory_(this) {
}

Animator::~Animator() = default;

void Animator::EnqueueTraceFlowId(uint64_t trace_flow_id) {
  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetUITaskRunner(),
      [self = weak_factory_.GetWeakPtr(), trace_flow_id] {
        if (!self) {
          return;
        }
        self->trace_flow_ids_.push_back(trace_flow_id);
        self->ScheduleMaybeClearTraceFlowIds();
      });
}

void Animator::BeginFrame(
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) {
  FML_DLOG(INFO) << "hi Animator::BeginFrame start";

  //  // test logs
  //  FML_DLOG(INFO) << "hi this is FML_DLOG(INFO)";
  //  FML_LOG(INFO) << "hi this is FML_LOG(INFO)";
  //  FML_DLOG(WARNING) << "hi this is FML_DLOG(WARNING)";
  //  FML_LOG(WARNING) << "hi this is FML_LOG(WARNING)";
  //  FML_DLOG(ERROR) << "hi this is FML_DLOG(ERROR)";
  //  FML_LOG(ERROR) << "hi this is FML_LOG(ERROR)";

  TRACE_EVENT_ASYNC_END0("flutter", "Frame Request Pending",
                         frame_request_number_);
  frame_request_number_++;

  frame_timings_recorder_ = std::move(frame_timings_recorder);
  frame_timings_recorder_->RecordBuildStart(fml::TimePoint::Now());

  TRACE_EVENT_WITH_FRAME_NUMBER(frame_timings_recorder_, "flutter",
                                "Animator::BeginFrame");
  while (!trace_flow_ids_.empty()) {
    uint64_t trace_flow_id = trace_flow_ids_.front();
    TRACE_FLOW_END("flutter", "PointerEvent", trace_flow_id);
    trace_flow_ids_.pop_front();
  }

  frame_scheduled_ = false;
  regenerate_layer_tree_ = false;
  //  pending_frame_semaphore_.Signal();
  pending_await_vsync_id_ = std::nullopt;

  FML_DLOG(INFO) << "hi Animator::BeginFrame producer_continuation_="
                 << static_cast<bool>(producer_continuation_);
  if (!producer_continuation_) {
    FML_DLOG(INFO) << "hi Animator::BeginFrame call producer_continuation_ := "
                      "pipeline->Produce";
    // We may already have a valid pipeline continuation in case a previous
    // begin frame did not result in an Animation::Render. Simply reuse that
    // instead of asking the pipeline for a fresh continuation.
    producer_continuation_ = layer_tree_pipeline_->Produce();
    FML_DLOG(INFO)
        << "hi Animator::BeginFrame after call produce producer_continuation_="
        << static_cast<bool>(producer_continuation_);

    if (!producer_continuation_) {
      // If we still don't have valid continuation, the pipeline is currently
      // full because the consumer is being too slow. Try again at the next
      // frame interval.
      TRACE_EVENT0("flutter", "PipelineFull");

      // NOTE MODIFIED allow continue running even if pipeline full #6042
      //      RequestFrame();
      //      FML_DLOG(INFO) << "hi Animator::BeginFrame return since
      //      pipelinefull"; return;
    }
  }

  // NOTE MODIFIED allow continue running even if pipeline full #6042
  //  // We have acquired a valid continuation from the pipeline and are ready
  //  // to service potential frame.
  //  FML_DCHECK(producer_continuation_);

  fml::tracing::TraceEventAsyncComplete(
      "flutter", "VsyncSchedulingOverhead",
      frame_timings_recorder_->GetVsyncStartTime(),
      frame_timings_recorder_->GetBuildStartTime());
  const fml::TimePoint frame_target_time =
      frame_timings_recorder_->GetVsyncTargetTime();
  dart_frame_deadline_ = frame_target_time.ToEpochDelta();
  uint64_t frame_number = frame_timings_recorder_->GetFrameNumber();
  delegate_.OnAnimatorBeginFrame(frame_target_time, frame_number);

  // NOTE MODIFIED ADD
  last_begin_frame_recorded_frame_target_time_ = frame_target_time;
  last_begin_frame_ending_time_ = fml::TimePoint::Now();

  if (!frame_scheduled_ && has_rendered_) {
    // Wait a tad more than 3 60hz frames before reporting a big idle period.
    // This is a heuristic that is meant to avoid giving false positives to the
    // VM when we are about to schedule a frame in the next vsync, the idea
    // being that if there have been three vsyncs with no frames it's a good
    // time to start doing GC work.
    task_runners_.GetUITaskRunner()->PostDelayedTask(
        [self = weak_factory_.GetWeakPtr()]() {
          if (!self) {
            return;
          }
          auto now = fml::TimeDelta::FromMicroseconds(Dart_TimelineGetMicros());
          // If there's a frame scheduled, bail.
          // If there's no frame scheduled, but we're not yet past the last
          // vsync deadline, bail.
          if (!self->frame_scheduled_ && now > self->dart_frame_deadline_) {
            TRACE_EVENT0("flutter", "BeginFrame idle callback");
            self->delegate_.OnAnimatorNotifyIdle(
                now + fml::TimeDelta::FromMilliseconds(100));
          }
        },
        kNotifyIdleTaskWaitTime);
  }
  FML_DLOG(INFO) << "hi Animator::BeginFrame end normally";
}

void Animator::Render(std::shared_ptr<flutter::LayerTree> layer_tree,
                      fml::TimePoint fallback_vsync_target_time) {
  FML_DLOG(INFO) << "hi Animator::Render start";

  has_rendered_ = true;
  last_layer_tree_size_ = layer_tree->frame_size();

  if (!frame_timings_recorder_) {
    // Framework can directly call render with a built scene.
    frame_timings_recorder_ = std::make_unique<FrameTimingsRecorder>();

    // NOTE MODIFIED
    const fml::TimePoint now = fml::TimePoint::Now();
    const fml::TimePoint vsync_target_time =
        // negative means do not provide
        fallback_vsync_target_time.ToEpochDelta().ToMicroseconds() > 0
            ? fallback_vsync_target_time
            : now;
    const fml::TimePoint placeholder_time = std::min(now, vsync_target_time);
    frame_timings_recorder_->RecordVsync(placeholder_time, vsync_target_time);
    frame_timings_recorder_->RecordBuildStart(placeholder_time);

    //    const fml::TimePoint placeholder_time = fml::TimePoint::Now();
    //    frame_timings_recorder_->RecordVsync(placeholder_time,
    //    placeholder_time);
    //    frame_timings_recorder_->RecordBuildStart(placeholder_time);

    FML_DLOG(INFO) << "populate frame_timings_recorder_ "
                   << " fallback_vsync_target_time="
                   << fallback_vsync_target_time.ToEpochDelta().ToMicroseconds()
                   << " placeholder_time="
                   << placeholder_time.ToEpochDelta().ToMicroseconds()
                   << " vsync_target_time="
                   << vsync_target_time.ToEpochDelta().ToMicroseconds();

    std::ostringstream info;
    info << "now=" << now.ToEpochDelta().ToMicroseconds()
         << " fallback_vsync_target_time="
         << fallback_vsync_target_time.ToEpochDelta().ToMicroseconds()
         << " placeholder_time="
         << placeholder_time.ToEpochDelta().ToMicroseconds()
         << " vsync_target_time="
         << vsync_target_time.ToEpochDelta().ToMicroseconds();
    TRACE_EVENT1("flutter", "populate frame_timings_recorder", "info",
                 info.str().c_str());
  }

  TRACE_EVENT_WITH_FRAME_NUMBER(frame_timings_recorder_, "flutter",
                                "Animator::Render");
  frame_timings_recorder_->RecordBuildEnd(fml::TimePoint::Now());

  delegate_.OnAnimatorUpdateLatestFrameTargetTime(
      frame_timings_recorder_->GetVsyncTargetTime());

  // ==== NOTE MODIFIED ADD start ====
  if (!producer_continuation_) {
    FML_DLOG(INFO) << "hi Animator::Render call EXTRA producer_continuation_ "
                      ":= pipeline->Produce";
    producer_continuation_ = layer_tree_pipeline_->Produce();
    FML_DLOG(INFO)
        << "hi Animator::Render after call produce producer_continuation_="
        << static_cast<bool>(producer_continuation_);
  }
  // ==== NOTE MODIFIED ADD end ====

  FML_DLOG(INFO) << "hi Animator::Render call producer_continuation_.Complete"
                 << " produce producer_continuation_="
                 << static_cast<bool>(producer_continuation_);
  auto layer_tree_item = std::make_unique<LayerTreeItem>(
      std::move(layer_tree), std::move(frame_timings_recorder_));
  // Commit the pending continuation.
  PipelineProduceResult result =
      producer_continuation_.Complete(std::move(layer_tree_item));

  if (!result.success) {
    FML_DLOG(INFO) << "hi Animator::Render return since !success";
    FML_DLOG(INFO) << "No pending continuation to commit";
    TRACE_EVENT0("flutter", "NoPendingContinuation");  // NOTE MODIFIED add
    return;
  }

  if (!result.is_first_item) {
    // It has been successfully pushed to the pipeline but not as the first
    // item. Eventually the 'Rasterizer' will consume it, so we don't need to
    // notify the delegate.
    FML_DLOG(INFO) << "hi Animator::Render return since !is_first_item";
    return;
  }

  FML_DLOG(INFO) << "hi Animator::Render call OnAnimatorDraw";
  delegate_.OnAnimatorDraw(layer_tree_pipeline_);

  FML_DLOG(INFO) << "hi Animator::Render end normally";
}

const std::weak_ptr<VsyncWaiter> Animator::GetVsyncWaiter() const {
  std::weak_ptr<VsyncWaiter> weak = waiter_;
  return weak;
}

bool Animator::CanReuseLastLayerTree() {
  return !regenerate_layer_tree_;
}

void Animator::DrawLastLayerTree(
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) {
  // This method is very cheap, but this makes it explicitly clear in trace
  // files.
  TRACE_EVENT0("flutter", "Animator::DrawLastLayerTree");
  //  pending_frame_semaphore_.Signal();
  pending_await_vsync_id_ = std::nullopt;

  // In this case BeginFrame doesn't get called, we need to
  // adjust frame timings to update build start and end times,
  // given that the frame doesn't get built in this case, we
  // will use Now() for both start and end times as an indication.
  const auto now = fml::TimePoint::Now();
  frame_timings_recorder->RecordBuildStart(now);
  frame_timings_recorder->RecordBuildEnd(now);
  delegate_.OnAnimatorDrawLastLayerTree(std::move(frame_timings_recorder));
}

static int next_request_frame_flow_id_ = 100;

void Animator::RequestFrame(
    bool regenerate_layer_tree,
    std::optional<fml::TimePoint> force_directly_call_next_vsync_target_time) {
  TRACE_EVENT1("flutter", "Animator::RequestFrame",
               "force_directly_call_next_vsync_target_time",
               std::to_string(force_directly_call_next_vsync_target_time
                                  .value_or(fml::TimePoint::FromTicks(-1))
                                  .ToEpochDelta()
                                  .ToMicroseconds())
                   .c_str());  // NOTE MODIFIED add
  auto request_frame_flow_id = next_request_frame_flow_id_++;
  TRACE_FLOW_BEGIN("flutter", "RequestFrame", request_frame_flow_id);

  FML_DLOG(INFO) << "hi Animator::RequestFrame start";
  if (regenerate_layer_tree) {
    // This event will be closed by BeginFrame. BeginFrame will only be called
    // if regenerating the layer tree. If a frame has been requested to update
    // an external texture, this will be false and no BeginFrame call will
    // happen.
    TRACE_EVENT_ASYNC_BEGIN0("flutter", "Frame Request Pending",
                             frame_request_number_);
    regenerate_layer_tree_ = true;
  }

  // NOTE in new semantics, when really in "force_directly_call" mode,
  //      should *cancel* previous pending call and schedule again
  //      instead of skip scheduling #6218
  //  if (!pending_frame_semaphore_.TryWait()) {
  if (!force_directly_call_next_vsync_target_time.has_value() &&
      pending_await_vsync_id_.load().has_value()) {
    // Multiple calls to Animator::RequestFrame will still result in a
    // single request to the VsyncWaiter.
    FML_DLOG(INFO) << "hi Animator::RequestFrame early return";
    TRACE_EVENT1("flutter", "RequestFrame-skip", "pending_await_vsync_id_",
                 std::to_string(pending_await_vsync_id_.load().value_or(-1))
                     .c_str());  // NOTE MODIFIED add
    return;
  }
  // TODO this is not atomic with above
  // NOTE when [pending_await_vsync_id_] is not null, this will effectively
  // cancel the previous one
  int curr_await_vsync_id = next_await_vsync_id_++;
  pending_await_vsync_id_ = curr_await_vsync_id;

  // The AwaitVSync is going to call us back at the next VSync. However, we want
  // to be reasonably certain that the UI thread is not in the middle of a
  // particularly expensive callout. We post the AwaitVSync to run right after
  // an idle. This does NOT provide a guarantee that the UI thread has not
  // started an expensive operation right after posting this message however.
  // To support that, we need edge triggered wakes on VSync.

  task_runners_.GetUITaskRunner()->PostTask(
      [self = weak_factory_.GetWeakPtr(),
       frame_request_number = frame_request_number_,
       force_directly_call_next_vsync_target_time,
       flow_id = request_frame_flow_id, curr_await_vsync_id]() {
        FML_DLOG(INFO)
            << "hi Animator::RequestFrame UITaskRunner PostTask callback start";
        if (!self) {
          return;
        }
        TRACE_EVENT_ASYNC_BEGIN0("flutter", "Frame Request Pending",
                                 frame_request_number);
        self->AwaitVSync(flow_id, force_directly_call_next_vsync_target_time,
                         curr_await_vsync_id);
        FML_DLOG(INFO)
            << "hi Animator::RequestFrame UITaskRunner PostTask callback end";
      });
  frame_scheduled_ = true;
  FML_DLOG(INFO) << "hi Animator::RequestFrame end";
}

const auto ONE_FRAME_DURATION = fml::TimeDelta::FromMicroseconds(16667);

fml::TimePoint NextVsync(fml::TimePoint now, fml::TimePoint arbitrary_vsync) {
  int n = static_cast<int>(std::ceil((now - arbitrary_vsync).ToMicrosecondsF() /
                                     ONE_FRAME_DURATION.ToMicrosecondsF()));
  return arbitrary_vsync + ONE_FRAME_DURATION * n;
}

std::optional<fml::TimePoint> AwaitVSyncShouldDirectlyCall(
    fml::TimePoint arbitrary_vsync_target_time,
    fml::TimePoint last_begin_frame_ending_time) {
  // #6087
  fml::TimePoint now = fml::TimePoint::Now();
  fml::TimePoint nearest_future_vsync_target_time =
      // NOTE should consider next vsync of [last_begin_frame_ending_time]
      // instead of next vsync of [now]
      // https://github.com/fzyzcjy/yplusplus/issues/6144#issuecomment-1274035115
      NextVsync(last_begin_frame_ending_time, arbitrary_vsync_target_time);
  // by experiment, 2ms is sometimes not enough, i.e. call VsyncWaiter at
  // 2ms before next vsync will lead to one frame missing
  // https://github.com/fzyzcjy/yplusplus/issues/6147#issuecomment-1274094709
  const auto THRESHOLD = fml::TimeDelta::FromMilliseconds(3);
  bool should_directly_call =
      (nearest_future_vsync_target_time - now) < THRESHOLD;

  std::optional<fml::TimePoint> ans =
      should_directly_call
          // note we need +one_frame
          // https://github.com/fzyzcjy/yplusplus/issues/6147#issuecomment-1274100126
          ? std::optional(nearest_future_vsync_target_time + ONE_FRAME_DURATION)
          : std::nullopt;

  std::ostringstream info;
  info << "should_directly_call=" << should_directly_call  //
       << ", now=" << now.ToEpochDelta().ToMicroseconds()  //
       << ", nearest_future_vsync_target_time="
       << nearest_future_vsync_target_time.ToEpochDelta().ToMicroseconds()
       << ", arbitrary_vsync_target_time="
       << arbitrary_vsync_target_time.ToEpochDelta().ToMicroseconds()
       << ", ans="
       << (ans.has_value() ? ans.value().ToEpochDelta().ToMicroseconds() : -1);
  TRACE_EVENT1("flutter", "AwaitVSync info", "info", info.str().c_str());

  return ans;
}

void Animator::AwaitVSync(
    uint64_t flow_id,
    std::optional<fml::TimePoint> force_directly_call_next_vsync_target_time,
    int curr_await_vsync_id) {
  TRACE_EVENT1("flutter", "Animator::AwaitVSync", "curr_await_vsync_id",
               std::to_string(curr_await_vsync_id).c_str());
  TRACE_FLOW_STEP("flutter", "RequestFrame", flow_id);

  std::optional<fml::TimePoint> next_vsync_target_time_if_should_directly_call =
      force_directly_call_next_vsync_target_time.has_value()
          ? force_directly_call_next_vsync_target_time.value()
          : (last_begin_frame_recorded_frame_target_time_.has_value()
                 ? AwaitVSyncShouldDirectlyCall(
                       last_begin_frame_recorded_frame_target_time_.value(),
                       last_begin_frame_ending_time_.value())
                 : std::nullopt);

  if (next_vsync_target_time_if_should_directly_call.has_value()) {
    const fml::TimePoint next_vsync_target_time =
        next_vsync_target_time_if_should_directly_call.value();

    // ref: how [Animator::Render] fills the recorder
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder =
        std::make_unique<FrameTimingsRecorder>();
    frame_timings_recorder->RecordVsync(
        next_vsync_target_time - ONE_FRAME_DURATION, next_vsync_target_time);

    task_runners_.GetUITaskRunner()->PostTask(fml::MakeCopyable(
        [self = weak_factory_.GetWeakPtr(),
         frame_timings_recorder = std::move(frame_timings_recorder), flow_id,
         curr_await_vsync_id]() mutable {
          TRACE_FLOW_END("flutter", "RequestFrame", flow_id);

          if (self) {
            // this is cancelled #6218
            if (self->pending_await_vsync_id_.load() != curr_await_vsync_id) {
              TRACE_EVENT0("flutter", "AwaitVSync-callback-cancelled");
              return;
            }

            if (self->CanReuseLastLayerTree()) {
              self->DrawLastLayerTree(std::move(frame_timings_recorder));
            } else {
              self->BeginFrame(std::move(frame_timings_recorder));
            }
          }
        }));
  } else {
    waiter_->AsyncWaitForVsync(
        [self = weak_factory_.GetWeakPtr(), flow_id, curr_await_vsync_id](
            std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) {
          TRACE_FLOW_END("flutter", "RequestFrame", flow_id);

          if (self) {
            // this is cancelled #6218
            if (self->pending_await_vsync_id_.load() != curr_await_vsync_id) {
              TRACE_EVENT0("flutter", "AwaitVSync-callback-cancelled");
              return;
            }

            if (self->CanReuseLastLayerTree()) {
              self->DrawLastLayerTree(std::move(frame_timings_recorder));
            } else {
              self->BeginFrame(std::move(frame_timings_recorder));
            }
          }
        });
  }

  if (has_rendered_) {
    // NOTE MODIFIED
    // in the old days, we are only OK until the next vsync
    // but now, we are OK until near the next of next vsync, because as long as
    // we have a few ms for smooth to run preemptRender, we will not miss frames
    delegate_.OnAnimatorNotifyIdle(
        dart_frame_deadline_ + fml::TimeDelta::FromMicroseconds(16667 - 3000));
    //    delegate_.OnAnimatorNotifyIdle(dart_frame_deadline_);
  }
}

void Animator::ScheduleSecondaryVsyncCallback(uintptr_t id,
                                              const fml::closure& callback) {
  waiter_->ScheduleSecondaryCallback(id, callback);
}

void Animator::ScheduleMaybeClearTraceFlowIds() {
  waiter_->ScheduleSecondaryCallback(
      reinterpret_cast<uintptr_t>(this), [self = weak_factory_.GetWeakPtr()] {
        if (!self) {
          return;
        }
        if (!self->frame_scheduled_ && !self->trace_flow_ids_.empty()) {
          TRACE_EVENT0("flutter",
                       "Animator::ScheduleMaybeClearTraceFlowIds - callback");
          while (!self->trace_flow_ids_.empty()) {
            auto flow_id = self->trace_flow_ids_.front();
            TRACE_FLOW_END("flutter", "PointerEvent", flow_id);
            self->trace_flow_ids_.pop_front();
          }
        }
      });
}

}  // namespace flutter

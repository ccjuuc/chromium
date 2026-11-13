// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_tracing.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "base/timer/timer.h"
#include "base/trace_event/trace_config.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/tracing_controller.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)

namespace xenon::ipc {
namespace {

struct TraceUsageRequest {
  ElectronTracingReply reply;
  base::OneShotTimer timeout;
};

// All file operations are serialized on a blocking worker. Trace chunks never
// accumulate as a single in-memory JSON string on the Browser UI sequence.
class TraceFileWriter {
 public:
  explicit TraceFileWriter(base::FilePath path) : path_(std::move(path)) {}

  void Write(std::unique_ptr<std::string> chunk,
             std::shared_ptr<std::atomic<size_t>> pending) {
    Open();
    if (error_.empty() &&
        !file_.WriteAtCurrentPosAndCheck(base::as_byte_span(*chunk))) {
      error_ = "EIO: unable to write trace output: " + path_.AsUTF8Unsafe();
    }
    bytes_ += chunk->size();
    pending->fetch_sub(chunk->size(), std::memory_order_relaxed);
  }

  base::DictValue Finish(bool success, std::string endpoint_error) {
    if (error_.empty() && !endpoint_error.empty()) {
      error_ = std::move(endpoint_error);
    }
    if (error_.empty() && (!success || bytes_ == 0)) {
      error_ =
          "ERR_OPERATION_FAILED: tracing did not produce a complete recording";
    }
    if (error_.empty() && !file_.Flush()) {
      error_ = "EIO: unable to flush trace output: " + path_.AsUTF8Unsafe();
    }
    file_.Close();
    return base::DictValue()
        .Set("path", path_.AsUTF8Unsafe())
        .Set("error", error_);
  }

 private:
  void Open() {
    if (file_.IsValid() || !error_.empty()) {
      return;
    }
    // StopTracing may reject the endpoint. Do not create/truncate anything
    // until Chromium actually delivers data or completes the accepted flush.
    if (path_.empty() && !base::CreateTemporaryFile(&path_)) {
      error_ = "EIO: unable to create a temporary trace file";
      return;
    }
    file_.Initialize(path_,
                     base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
    if (!file_.IsValid()) {
      error_ = "EIO: unable to open trace output: " + path_.AsUTF8Unsafe();
    }
  }
  base::FilePath path_;
  base::File file_;
  std::string error_;
  size_t bytes_ = 0;
};

class TraceFileEndpoint final
    : public content::TracingController::TraceDataEndpoint {
 public:
  TraceFileEndpoint(base::FilePath path,
                    base::OnceCallback<void(base::DictValue)> done)
      : writer_(base::ThreadPool::CreateSequencedTaskRunner(
                    {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
                     base::TaskShutdownBehavior::BLOCK_SHUTDOWN}),
                std::move(path)),
        done_(std::move(done)) {}

  void ReceiveTraceChunk(std::unique_ptr<std::string> chunk) override {
    if (!error_.empty()) {
      return;
    }
    constexpr size_t kMaxPendingBytes = 16 * 1024 * 1024;
    const auto previous =
        pending_->fetch_add(chunk->size(), std::memory_order_relaxed);
    if (chunk->size() > kMaxPendingBytes ||
        previous > kMaxPendingBytes - chunk->size()) {
      pending_->fetch_sub(chunk->size(), std::memory_order_relaxed);
      error_ =
          "ERR_RESOURCE_BUSY: trace output writer exceeded its pending byte "
          "limit";
      return;
    }
    writer_.AsyncCall(&TraceFileWriter::Write)
        .WithArgs(std::move(chunk), pending_);
  }

  void ReceivedTraceFinalContents() override {
    // This endpoint requires an explicit completion status from the native
    // controller. An old backend cannot certify a complete trace.
    ReceivedTraceFinalContentsWithStatus(false);
  }

  void ReceivedTraceFinalContentsWithStatus(bool success) override {
    writer_.AsyncCall(&TraceFileWriter::Finish)
        .WithArgs(success, std::move(error_))
        .Then(std::move(done_));
  }

 private:
  ~TraceFileEndpoint() override = default;
  base::SequenceBound<TraceFileWriter> writer_;
  base::OnceCallback<void(base::DictValue)> done_;
  std::shared_ptr<std::atomic<size_t>> pending_ =
      std::make_shared<std::atomic<size_t>>(0);
  std::string error_;
};

class ElectronTracingBroker {
 public:
  static ElectronTracingBroker& Get() {
    static base::NoDestructor<ElectronTracingBroker> broker;
    return *broker;
  }

  void Call(const std::string& owner,
            const base::Value& request,
            ElectronTracingReply reply) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    const auto* args = request.GetIfDict();
    const auto* method = args ? args->FindString("method") : nullptr;
    if (!method) {
      std::move(reply).Run(base::Value(),
                           "ERR_INVALID_ARG_TYPE: tracing requires method");
      return;
    }
    auto* controller = content::TracingController::GetInstance();
    if (*method == "getCategories") {
      auto split = base::SplitOnceCallback(std::move(reply));
      if (!controller->GetCategories(base::BindOnce(
              [](ElectronTracingReply reply,
                 const std::set<std::string>& categories) {
                base::ListValue values;
                for (const auto& category : categories) {
                  values.Append(category);
                }
                std::move(reply).Run(base::Value(std::move(values)), "");
              },
              std::move(split.first)))) {
        std::move(split.second)
            .Run(base::Value(),
                 "ERR_OPERATION_FAILED: unable to query trace categories");
      }
      return;
    }
    if (*method == "getTraceBufferUsage") {
      auto pending = std::make_shared<TraceUsageRequest>();
      pending->reply = std::move(reply);
      auto complete = base::BindRepeating(
          [](std::shared_ptr<TraceUsageRequest> pending, float percentage,
             size_t count) {
            if (!pending->reply) {
              return;
            }
            pending->timeout.Stop();
            if (!std::isfinite(percentage)) {
              std::move(pending->reply)
                  .Run({},
                       "ERR_OPERATION_FAILED: unable to query trace buffer");
              return;
            }
            std::move(pending->reply)
                .Run(base::Value(base::DictValue()
                                     .Set("percentage", percentage)
                                     .Set("value", static_cast<double>(count))),
                     "");
          },
          pending);
      const float invalid = std::numeric_limits<float>::quiet_NaN();
      pending->timeout.Start(FROM_HERE, base::Seconds(30),
                             base::BindOnce(complete, invalid, 0u));
      if (!controller->GetTraceBufferUsage(
              mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                  base::BindOnce(complete), invalid, size_t{0}))) {
        complete.Run(invalid, 0);
      }
      return;
    }
    if (*method == "startRecording") {
      if (!controller->IsTracing() && !stopping_ && !start_reply_) {
        owner_.clear();
      }
      if (!owner_.empty() || controller->IsTracing()) {
        std::move(reply).Run(
            base::Value(),
            "ERR_RESOURCE_BUSY: a trace recording already exists");
        return;
      }
      const auto* options = args->FindDict("options");
      if (!options) {
        std::move(reply).Run(
            base::Value(),
            "ERR_INVALID_ARG_TYPE: trace options must be an object");
        return;
      }
      base::trace_event::TraceConfig config;
      const auto* categories = options->FindString("categoryFilter");
      const auto* trace_options = options->FindString("traceOptions");
      if (categories && trace_options) {
        config = base::trace_event::TraceConfig(*categories, *trace_options);
      } else {
        config = base::trace_event::TraceConfig(*options);
      }
      owner_ = owner;
      const auto recording = ++recording_;
      start_reply_ = std::move(reply);
      if (!controller->StartTracing(
              config, base::BindOnce(&ElectronTracingBroker::Started,
                                     base::Unretained(this), recording))) {
        owner_.clear();
        std::move(start_reply_)
            .Run(base::Value(),
                 "ERR_OPERATION_FAILED: unable to start tracing");
      } else {
        controller_generation_ = controller->GetRecordingGeneration();
        if (!controller_generation_) {
          // Start returned on this UI stack; no other caller can have replaced
          // the just-created session yet. Refuse backends without identity.
          controller->StopTracing(nullptr);
          owner_.clear();
          if (start_reply_) {
            std::move(start_reply_)
                .Run(
                    {},
                    "ERR_NOT_SUPPORTED: trace session identity is unavailable");
          }
          return;
        }
        if (!start_reply_) {
          return;
        }
        // TracingController has no error callback when its service disconnects
        // during startup. Bound that failure instead of leaking a JS Promise.
        start_timer_.Start(FROM_HERE, base::Seconds(30),
                           base::BindOnce(&ElectronTracingBroker::StartTimedOut,
                                          base::Unretained(this), recording));
      }
      return;
    }
    if (*method == "stopRecording") {
      if (owner_ != owner || stopping_ || start_reply_ ||
          !OwnsRecording(controller)) {
        std::move(reply).Run(
            base::Value(),
            "ERR_INVALID_STATE: no owned recording is available");
        return;
      }
      const auto* supplied_path = args->FindString("path");
      if (!supplied_path || supplied_path->find('\0') != std::string::npos) {
        std::move(reply).Run(base::Value(),
                             "ERR_INVALID_ARG_TYPE: invalid trace output path");
        return;
      }
      const auto path = base::FilePath::FromUTF8Unsafe(*supplied_path);
      if (!path.empty() && !path.IsAbsolute()) {
        std::move(reply).Run(
            base::Value(),
            "ERR_INVALID_ARG_VALUE: trace output must be absolute");
        return;
      }
      stopping_ = true;
      auto split = base::SplitOnceCallback(std::move(reply));
      auto endpoint = base::MakeRefCounted<TraceFileEndpoint>(
          path,
          base::BindOnce(
              [](ElectronTracingBroker* broker, uint64_t recording,
                 ElectronTracingReply reply, base::DictValue result) {
                if (broker->recording_ == recording) {
                  broker->stopping_ = false;
                  broker->owner_.clear();
                }
                std::move(reply).Run(base::Value(*result.FindString("path")),
                                     *result.FindString("error"));
              },
              base::Unretained(this), recording_, std::move(split.first)));
      if (!controller->StopTracing(endpoint)) {
        stopping_ = false;
        std::move(split.second)
            .Run(base::Value(), "ERR_OPERATION_FAILED: unable to stop tracing");
      }
      return;
    }
    std::move(reply).Run(base::Value(),
                         "ERR_NOT_SUPPORTED: unknown contentTracing method");
  }

  void Close(const std::string& owner) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (owner_ != owner || stopping_) {
      return;
    }
    // A stopped owner no longer receives callbacks. Chromium owns the flush
    // operation and will reject a new recording while its stop is pending.
    owner_.clear();
    start_timer_.Stop();
    if (start_reply_) {
      std::move(start_reply_)
          .Run(base::Value(), "ERR_ABORTED: tracing owner stopped");
    }
    auto* controller = content::TracingController::GetInstance();
    if (OwnsRecording(controller)) {
      controller->StopTracing(nullptr);
    }
  }

 private:
  bool OwnsRecording(content::TracingController* controller) const {
    return controller_generation_ && controller->IsTracing() &&
           controller->GetRecordingGeneration() == controller_generation_;
  }
  void Started(uint64_t recording) {
    if (recording != recording_ || !start_reply_) {
      return;
    }
    start_timer_.Stop();
    std::move(start_reply_).Run(base::Value(), "");
  }

  void StartTimedOut(uint64_t recording) {
    if (recording != recording_ || !start_reply_) {
      return;
    }
    auto reply = std::move(start_reply_);
    Close(owner_);
    std::move(reply).Run(base::Value(),
                         "ERR_OPERATION_FAILED: tracing startup timed out");
  }

  std::string owner_;
  bool stopping_ = false;
  uint64_t recording_ = 0;
  uint64_t controller_generation_ = 0;
  ElectronTracingReply start_reply_;
  base::OneShotTimer start_timer_;
};

}  // namespace

void CallElectronTracing(const std::string& owner,
                         const base::Value& request,
                         ElectronTracingReply reply) {
  ElectronTracingBroker::Get().Call(owner, request, std::move(reply));
}

void CloseElectronTracing(const std::string& owner) {
  ElectronTracingBroker::Get().Close(owner);
}

}  // namespace xenon::ipc

#else

namespace xenon::ipc {
void CallElectronTracing(const std::string&,
                         const base::Value&,
                         ElectronTracingReply reply) {
  std::move(reply).Run({}, "ERR_NOT_SUPPORTED: Electron tracing is disabled");
}
void CloseElectronTracing(const std::string&) {}
}  // namespace xenon::ipc

#endif  // BUILDFLAG(ENABLE_XENON_SERVICE)

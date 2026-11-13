// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_worker_engine.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_observer.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread.h"
#include "gin/public/isolate_holder.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMegabyte = 1024 * 1024;
constexpr size_t kMaximumPendingBytes = 64 * kMegabyte;
constexpr size_t kMaximumWorkers = 32;
void ReapThread(std::unique_ptr<base::Thread> thread) {
  // Stop joins. Never block the application's JS sequence on a worker.
  thread->DetachFromSequence();
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(
          [](std::unique_ptr<base::Thread> thread) { thread->Stop(); },
          std::move(thread)));
}
}  // namespace

class WorkerEngine::State final : public base::RefCountedThreadSafe<State>,
                                  public WorkerControl,
                                  public base::TaskObserver {
 public:
  State(int id,
        EnvironmentFactory factory,
        scoped_refptr<base::SingleThreadTaskRunner> owner,
        base::WeakPtr<WorkerEngine> engine)
      : id_(id),
        factory_(std::move(factory)),
        owner_(std::move(owner)),
        engine_(std::move(engine)) {}
  void Start(WorkerOptions options,
             std::unique_ptr<WorkerMessage> data,
             std::unique_ptr<v8::Isolate::CreateParams> params) {
    runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
    base::CurrentThread::Get().AddTaskObserver(this);
    observing_ = true;
    if (terminated_.load()) {
      Finish();
      return;
    }
    environment_ = factory_.Run();
    Send("online");
    std::string error;
    if (!environment_->Initialize(std::move(options), std::move(data),
                                  std::move(params), this, &error) &&
        !terminated_.load()) {
      base::DictValue details;
      details.Set("name", "Error");
      details.Set("message", error);
      details.Set("stack", error);
      Fail(std::move(details));
    }
    CheckIdle();
  }
  bool Post(std::unique_ptr<PreparedWorkerMessage> prepared,
            v8::Isolate* sender,
            scoped_refptr<base::SingleThreadTaskRunner> runner,
            std::string* error) {
    const size_t size = prepared->message_size();
    {
      base::AutoLock lock(lock_);
      if (draining_ || closed_ || terminated_.load()) {
        return false;
      }
      if (size > kMaximumPendingBytes - pending_bytes_) {
        *error = "ERR_BUFFER_TOO_LARGE: Worker message queue exceeds 64 MiB";
        return false;
      }
      // Accept the message and reserve space before committing transfers.
      pending_bytes_ += size;
    }
    // V8 may allocate when reporting a failed transfer. Never hold the state
    // lock across V8 calls: its near-heap-limit callback also takes that lock.
    auto message = prepared->Commit(sender);
    if (!message) {
      {
        base::AutoLock lock(lock_);
        pending_bytes_ -= size;
      }
      runner->PostTask(FROM_HERE, base::BindOnce(&State::WakeForTermination,
                                                 base::WrapRefCounted(this)));
      return false;
    }
    return runner->PostTask(
        FROM_HERE, base::BindOnce(&State::Deliver, base::WrapRefCounted(this),
                                  std::move(message), size));
  }
  void Terminate() {
    base::AutoLock lock(lock_);
    // parentPort.close() alone does not stop execution. Only completed
    // execution may ignore a late terminate and preserve its real exit code.
    if (exiting_) {
      return;
    }
    terminated_.store(true);
    exit_code_ = 1;
    if (isolate_) {
      isolate_->TerminateExecution();
    }
  }
  void WakeForTermination() { CheckIdle(); }
  void PublishIsolate(v8::Isolate* isolate) override {
    base::AutoLock lock(lock_);
    isolate_ = isolate;
    if (!isolate_) {
      // No more JavaScript can execute. Until this point even an exit
      // listener may need to be interrupted by Worker.terminate().
      exiting_ = true;
    }
    if (isolate_) {
      isolate_->AddNearHeapLimitCallback(&NearHeapLimit, this);
      if (terminated_.load()) {
        isolate_->TerminateExecution();
      }
    }
  }
  bool PostToParent(std::unique_ptr<PreparedWorkerMessage> prepared,
                    v8::Isolate* sender,
                    std::string* error) override {
    const size_t size = prepared->message_size();
    {
      base::AutoLock lock(lock_);
      if (closed_ || terminated_.load()) {
        return false;
      }
      if (size > kMaximumPendingBytes - outbound_bytes_) {
        *error =
            "ERR_BUFFER_TOO_LARGE: Worker parent message queue exceeds 64 MiB";
        return false;
      }
      outbound_bytes_ += size;
    }
    // Releases the reservation whether the owner consumes this message or
    // discards the queued task while shutting down. Its WeakPtr need not live.
    base::ScopedClosureRunner reservation(base::BindOnce(
        &State::ReleaseOutbound, base::WrapRefCounted(this), size));
    Event event;
    event.id = id_;
    event.type = "message";
    event.message = prepared->Commit(sender);
    if (!event.message) {
      return false;
    }
    return owner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<WorkerEngine> engine, Event queued_event,
               base::ScopedClosureRunner queue_reservation) {
              queue_reservation.RunAndReset();
              if (engine) {
                engine->OnEvent(std::move(queued_event));
              }
            },
            engine_, std::move(event), std::move(reservation)));
  }
  void SetParentPortReferenced(bool referenced) override {
    port_referenced_ = referenced;
  }
  void CloseParentPort() override {
    {
      base::AutoLock lock(lock_);
      if (closed_) {
        return;
      }
      closed_ = true;
    }
    port_referenced_ = false;
    runner_->PostTask(FROM_HERE, base::BindOnce(&State::NotifyPortClosed,
                                                base::WrapRefCounted(this)));
  }
  void Exit(int code) override {
    base::AutoLock lock(lock_);
    exit_code_ = code;
    terminated_.store(true);
    if (isolate_) {
      isolate_->TerminateExecution();
    }
  }
  void Fail(base::DictValue error, int exit_code = 1) override {
    if (!failed_) {
      failed_ = true;
      Send("error", base::Value(std::move(error)));
    }
    Exit(exit_code);
  }
  void WillProcessTask(const base::PendingTask&, bool) override {}
  void DidProcessTask(const base::PendingTask&) override { CheckIdle(); }

 private:
  friend class base::RefCountedThreadSafe<State>;
  ~State() override = default;
  void ReleaseOutbound(size_t size) {
    base::AutoLock lock(lock_);
    outbound_bytes_ -= size;
  }
  static size_t NearHeapLimit(void* data, size_t current, size_t initial) {
    auto* self = static_cast<State*>(data);
    self->out_of_memory_ = true;
    self->Exit(1);
    // V8 needs a reserve to unwind execution instead of killing the process.
    return current + 8 * kMegabyte;
  }
  void Deliver(std::unique_ptr<WorkerMessage> message, size_t size) {
    bool closed;
    {
      base::AutoLock lock(lock_);
      pending_bytes_ -= size;
      closed = closed_;
    }
    if (!closed && !terminated_.load() && environment_) {
      environment_->Deliver(std::move(message));
    }
  }
  void NotifyPortClosed() {
    if (environment_) {
      environment_->ParentPortClosed();
    }
  }
  void CheckIdle() {
    if (finishing_ || !environment_) {
      return;
    }
    bool pending;
    {
      base::AutoLock lock(lock_);
      pending = pending_bytes_ != 0;
    }
    if (terminated_.load() ||
        (!pending && !port_referenced_ && !environment_->HasActiveWork())) {
      // Defer disposal until after V8 scopes and task observers have unwound.
      finishing_ = true;
      runner_->PostTask(FROM_HERE, base::BindOnce(&State::Finish,
                                                  base::WrapRefCounted(this)));
    }
  }
  void Finish() {
    // A parentPort close handler may have created a timer after CheckIdle
    // scheduled this disposal task. Recheck before ending natural execution.
    if (!terminated_.load() && environment_) {
      bool pending;
      {
        base::AutoLock lock(lock_);
        pending = pending_bytes_ != 0;
      }
      if (pending || port_referenced_ || environment_->HasActiveWork()) {
        finishing_ = false;
        return;
      }
      const int natural_exit_code = environment_->BeforeExit();
      if (!terminated_.load()) {
        if (port_referenced_ || environment_->HasActiveWork()) {
          finishing_ = false;
          return;
        }
        base::AutoLock lock(lock_);
        // A parent can enqueue while beforeExit or its microtasks run.
        // Make rejecting new input atomic with this final pending check.
        if (!terminated_.load()) {
          if (pending_bytes_ != 0) {
            finishing_ = false;
            return;
          }
          exit_code_ = natural_exit_code;
          draining_ = true;
        }
      }
    }
    if (observing_) {
      base::CurrentThread::Get().RemoveTaskObserver(this);
      observing_ = false;
    }
    {
      base::AutoLock lock(lock_);
      // Stop inbound delivery, but keep parentPort.postMessage available
      // during natural exit listeners, as in Node. Explicit port closure
      // and forced termination still reject outbound messages.
      draining_ = true;
      if (isolate_) {
        isolate_->CancelTerminateExecution();
      }
    }
    if (out_of_memory_ && !failed_) {
      base::DictValue error;
      error.Set("name", "Error");
      error.Set("message", "Worker terminated after reaching its memory limit");
      error.Set("code", "ERR_WORKER_OUT_OF_MEMORY");
      Send("error", base::Value(std::move(error)));
    }
    int code;
    {
      base::AutoLock lock(lock_);
      code = exit_code_;
    }
    if (environment_) {
      const int final_code = environment_->Shutdown(code, terminated_.load());
      {
        base::AutoLock lock(lock_);
        if (!terminated_.load()) {
          exit_code_ = final_code;
        }
      }
      xenon::NativeRuntimeRetirementOptions options;
      options.on_stalled = base::BindOnce(
          [](int id, std::string detail) {
            LOG(ERROR) << "Worker " << id
                       << " native runtime failed to drain: " << detail;
          },
          id_);
      // This reference retains WorkerControl while the environment drains.
      // The engine keeps the thread running until CompleteFinish emits exit.
      options.on_complete = base::BindOnce(&State::CompleteFinish,
                                           base::WrapRefCounted(this));
      xenon::RetireNativeRuntime(std::move(environment_), std::move(options));
      return;
    }
    CompleteFinish();
  }
  void CompleteFinish() {
    int code;
    {
      base::AutoLock lock(lock_);
      CHECK(!isolate_);
      closed_ = true;
      exiting_ = true;
      code = exit_code_;
    }
    Send("exit", base::Value(code));
  }
  void Send(std::string type, base::Value payload = base::Value()) {
    Event event;
    event.id = id_;
    event.type = std::move(type);
    event.payload = std::move(payload);
    owner_->PostTask(FROM_HERE, base::BindOnce(&WorkerEngine::OnEvent, engine_,
                                               std::move(event)));
  }
  const int id_;
  EnvironmentFactory factory_;
  scoped_refptr<base::SingleThreadTaskRunner> owner_;
  base::WeakPtr<WorkerEngine> engine_;
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  std::unique_ptr<WorkerEnvironment> environment_;
  base::Lock lock_;
  raw_ptr<v8::Isolate> isolate_ GUARDED_BY(lock_) = nullptr;
  size_t pending_bytes_ GUARDED_BY(lock_) = 0;
  size_t outbound_bytes_ GUARDED_BY(lock_) = 0;
  bool draining_ GUARDED_BY(lock_) = false;
  bool closed_ GUARDED_BY(lock_) = false;
  bool exiting_ GUARDED_BY(lock_) = false;
  int exit_code_ GUARDED_BY(lock_) = 0;
  std::atomic<bool> terminated_{false};
  bool port_referenced_ = false;
  bool observing_ = false;
  bool finishing_ = false;
  bool failed_ = false;
  bool out_of_memory_ = false;
};

struct WorkerEngine::Entry {
  std::unique_ptr<base::Thread> thread;
  scoped_refptr<State> state;
  bool referenced = true;
};
WorkerEngine::WorkerEngine(EnvironmentFactory factory, EventCallback callback)
    : factory_(std::move(factory)),
      callback_(std::move(callback)),
      owner_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {}
WorkerEngine::~WorkerEngine() {
  Shutdown();
  CHECK(IsDrained()) << "Worker engine destroyed before native retirement";
}
bool WorkerEngine::Create(WorkerOptions* options,
                          std::unique_ptr<PreparedWorkerMessage> data,
                          v8::Isolate* sender,
                          std::string* error) {
  if (stopped_ || workers_.size() >= kMaximumWorkers) {
    *error = "Worker limit reached or application is shutting down";
    return false;
  }
  auto params = gin::IsolateHolder::getDefaultIsolateParams();
  params->allow_atomics_wait = true;
  if (options->old_generation_mb > 0) {
    params->constraints.set_max_old_generation_size_in_bytes(
        options->old_generation_mb * kMegabyte);
  }
  if (options->young_generation_mb > 0) {
    params->constraints.set_max_young_generation_size_in_bytes(
        options->young_generation_mb * kMegabyte);
  }
  options->resource_limits.Set(
      "maxOldGenerationSizeMb",
      static_cast<double>(
          params->constraints.max_old_generation_size_in_bytes()) /
          kMegabyte);
  options->resource_limits.Set(
      "maxYoungGenerationSizeMb",
      static_cast<double>(
          params->constraints.max_young_generation_size_in_bytes()) /
          kMegabyte);
  options->resource_limits.Set("stackSizeMb", options->stack_mb);
  options->id = next_id_++;
  if (options->name.empty()) {
    options->name = "Worker " + std::to_string(options->id);
  }
  auto entry = std::make_unique<Entry>();
  entry->thread = std::make_unique<base::Thread>(options->name);
  base::Thread::Options thread_options;
  thread_options.stack_size = options->stack_mb * kMegabyte;
  if (!entry->thread->StartWithOptions(std::move(thread_options))) {
    *error = "Unable to start Worker thread";
    return false;
  }
  entry->state = base::MakeRefCounted<State>(
      options->id, factory_, owner_runner_, weak_factory_.GetWeakPtr());
  // This newly started thread cannot stop before its initialization task.
  auto message = data->Commit(sender);
  if (!message) {
    ReapThread(std::move(entry->thread));
    return false;
  }
  WorkerOptions thread_config;
  thread_config.id = options->id;
  thread_config.source = options->source;
  thread_config.filename = options->filename;
  thread_config.app_path = options->app_path;
  thread_config.executable_path = options->executable_path;
  thread_config.resources_directory = options->resources_directory;
  thread_config.working_directory = options->working_directory;
  thread_config.argv = options->argv;
  thread_config.environment = options->environment.Clone();
  thread_config.name = options->name;
  thread_config.eval = options->eval;
  thread_config.track_unmanaged_fds = options->track_unmanaged_fds;
  thread_config.stack_mb = options->stack_mb;
  thread_config.resource_limits = options->resource_limits.Clone();
  thread_config.network_factory = std::move(options->network_factory);
  entry->thread->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&State::Start, entry->state, std::move(thread_config),
                     std::move(message), std::move(params)));
  workers_.emplace(options->id, std::move(entry));
  return true;
}
bool WorkerEngine::Post(int id,
                        std::unique_ptr<PreparedWorkerMessage> message,
                        v8::Isolate* sender,
                        std::string* error) {
  auto it = workers_.find(id);
  return it != workers_.end() &&
         it->second->state->Post(std::move(message), sender,
                                 it->second->thread->task_runner(), error);
}
bool WorkerEngine::Control(int id, const std::string& command) {
  auto it = workers_.find(id);
  if (it == workers_.end()) {
    return false;
  }
  if (command == "terminate") {
    it->second->state->Terminate();
    it->second->thread->task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(&State::WakeForTermination, it->second->state));
  } else if (command == "ref" || command == "unref") {
    it->second->referenced = command == "ref";
  } else {
    return false;
  }
  return true;
}
void WorkerEngine::OnEvent(Event event) {
  auto it = workers_.find(event.id);
  if (it == workers_.end()) {
    return;
  }
  if (event.type == "exit") {
    ReapThread(std::move(it->second->thread));
    workers_.erase(it);
  }
  if (!stopped_) {
    callback_.Run(std::move(event));
  }
}
void WorkerEngine::Shutdown() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  for (auto& [id, entry] : workers_) {
    entry->state->Terminate();
    entry->thread->task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&State::WakeForTermination, entry->state));
  }
}
bool WorkerEngine::IsDrained() const {
  return workers_.empty();
}
}  // namespace xenon::ipc

// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_WORKER_ENGINE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_WORKER_ENGINE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "v8/include/v8-isolate.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_worker_message.h"
#include "xenon_overlay/chrome/browser/napi/native_runtime_retirement.h"

namespace base {
class SingleThreadTaskRunner;
class Thread;
}  // namespace base
namespace xenon::ipc {

// All paths and environment data are snapshotted before the thread starts.
// No V8 handles, bound Mojo remotes, or parent-sequence callbacks cross this
// boundary. The pending network factory is cloned on the parent and bound on
// the Worker sequence, as required by SharedURLLoaderFactory.
struct WorkerOptions {
  int id = 0;
  std::string source;
  base::FilePath filename;
  base::FilePath app_path;
  base::FilePath executable_path;
  base::FilePath resources_directory;
  base::FilePath working_directory;
  std::vector<std::string> argv;
  base::DictValue environment;
  std::string name;
  bool eval = false;
  bool track_unmanaged_fds = true;
  double old_generation_mb = 0;
  double young_generation_mb = 0;
  double stack_mb = 4;
  base::DictValue resource_limits;
  std::unique_ptr<network::PendingSharedURLLoaderFactory> network_factory;
};

class WorkerControl {
 public:
  virtual ~WorkerControl() = default;
  // Publish before executing any JS; clear immediately before isolate disposal.
  virtual void PublishIsolate(v8::Isolate* isolate) = 0;
  virtual bool PostToParent(std::unique_ptr<PreparedWorkerMessage> message,
                            v8::Isolate* isolate,
                            std::string* error) = 0;
  virtual void SetParentPortReferenced(bool referenced) = 0;
  virtual void CloseParentPort() = 0;
  virtual void Exit(int code) = 0;
  virtual void Fail(base::DictValue error, int exit_code = 1) = 0;
};

// Implemented by the shared Node environment in its explicit worker role.
class WorkerEnvironment : public xenon::NativeRuntimeShutdown {
 public:
  ~WorkerEnvironment() override = default;
  virtual bool Initialize(WorkerOptions options,
                          std::unique_ptr<WorkerMessage> data,
                          std::unique_ptr<v8::Isolate::CreateParams> params,
                          WorkerControl* control,
                          std::string* error) = 0;
  virtual void Deliver(std::unique_ptr<WorkerMessage> message) = 0;
  virtual void ParentPortClosed() = 0;
  virtual bool HasActiveWork() const = 0;
  // beforeExit may schedule work; the engine checks HasActiveWork again.
  virtual int BeforeExit() = 0;
  // Runs exit hooks and starts shutdown, without promising native work has
  // drained. Ownership then passes to the shared runtime retirement barrier.
  virtual int Shutdown(int exit_code, bool terminated) = 0;
};

class WorkerEngine {
 public:
  struct Event {
    int id = 0;
    std::string type;
    base::Value payload;
    std::unique_ptr<WorkerMessage> message;
  };
  using EventCallback = base::RepeatingCallback<void(Event)>;
  using EnvironmentFactory =
      base::RepeatingCallback<std::unique_ptr<WorkerEnvironment>()>;
  WorkerEngine(EnvironmentFactory factory, EventCallback callback);
  ~WorkerEngine();
  // Returns the effective resource limits and thread name through options.
  // Failed creation leaves every transfer buffer attached.
  bool Create(WorkerOptions* options,
              std::unique_ptr<PreparedWorkerMessage> data,
              v8::Isolate* sender,
              std::string* error);
  bool Post(int id,
            std::unique_ptr<PreparedWorkerMessage> message,
            v8::Isolate* sender,
            std::string* error);
  bool Control(int id, const std::string& command);
  void Shutdown();
  // Parent runtime retirement must keep this engine and its Worker sequences
  // alive until every native environment has completed its own retirement.
  bool IsDrained() const;

 private:
  class State;
  struct Entry;
  void OnEvent(Event event);
  EnvironmentFactory factory_;
  EventCallback callback_;
  scoped_refptr<base::SingleThreadTaskRunner> owner_runner_;
  std::map<int, std::unique_ptr<Entry>> workers_;
  int next_id_ = 1;
  bool stopped_ = false;
  base::WeakPtrFactory<WorkerEngine> weak_factory_{this};
};
}  // namespace xenon::ipc
#endif

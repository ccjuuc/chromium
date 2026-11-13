// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_POLICY_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_POLICY_H_

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"

namespace xenon::ipc {

// UI-sequence policy state for one authenticated application service
// generation. Application closures stay in that service. Only registration ids
// and pending decisions are retained here; permission answers are never guessed
// or cached.
class ElectronPermissionPolicy
    : public base::RefCounted<ElectronPermissionPolicy> {
 public:
  using CheckCallback =
      base::RepeatingCallback<base::expected<bool, std::string>(base::Value)>;
  using EventCallback = base::RepeatingCallback<void(base::Value)>;
  using ValidCallback = base::RepeatingCallback<bool()>;

  ElectronPermissionPolicy(CheckCallback check,
                           EventCallback event,
                           base::RepeatingClosure changed,
                           base::TimeDelta request_timeout);

  base::expected<void, std::string> SetHandler(const std::string& partition,
                                               const std::string& kind,
                                               int registration,
                                               bool enabled);
  base::expected<bool, std::string> Check(const std::string& partition,
                                          base::DictValue request,
                                          ValidCallback valid,
                                          bool default_granted);
  void Request(const std::string& partition,
               base::DictValue request,
               ValidCallback valid,
               base::OnceCallback<void(bool)> callback);
  void RequestDisplay(const std::string& partition,
                      base::DictValue request,
                      ValidCallback valid,
                      base::OnceCallback<void(base::DictValue)> callback);
  bool Respond(const std::string& partition,
               int registration,
               int request_id,
               bool granted);
  bool RespondDisplay(const std::string& partition,
                      int registration,
                      int request_id,
                      base::DictValue selection);
  void Close();
  bool closed() const { return closed_; }

 private:
  friend class base::RefCounted<ElectronPermissionPolicy>;
  struct Registration {
    int id = 0;
    bool enabled = false;
  };
  struct Pending;
  ~ElectronPermissionPolicy();
  void RequestValue(const std::string& partition,
                    const std::string& kind,
                    base::DictValue request,
                    ValidCallback valid,
                    base::OnceCallback<void(base::Value)> callback);
  bool RespondValue(const std::string& partition,
                    const std::string& kind,
                    int registration,
                    int request_id,
                    base::Value value);
  void Finish(int request_id, base::Value value);
  void CancelPartition(const std::string& partition, const std::string& kind);

  const CheckCallback check_;
  const EventCallback event_;
  const base::RepeatingClosure changed_;
  const base::TimeDelta request_timeout_;
  bool closed_ = false;
  int next_request_id_ = 1;
  std::map<std::pair<std::string, std::string>, Registration> registrations_;
  std::map<int, std::unique_ptr<Pending>> pending_;
  base::WeakPtrFactory<ElectronPermissionPolicy> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_POLICY_H_

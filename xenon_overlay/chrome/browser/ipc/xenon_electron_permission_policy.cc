// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_policy.h"

#include <limits>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/timer/timer.h"

namespace xenon::ipc {

struct ElectronPermissionPolicy::Pending {
  std::string partition;
  std::string kind;
  int registration;
  ValidCallback valid;
  base::OnceCallback<void(base::Value)> callback;
  base::OneShotTimer deadline;
};

ElectronPermissionPolicy::ElectronPermissionPolicy(
    CheckCallback check,
    EventCallback event,
    base::RepeatingClosure changed,
    base::TimeDelta request_timeout)
    : check_(std::move(check)),
      event_(std::move(event)),
      changed_(std::move(changed)),
      request_timeout_(request_timeout) {
  CHECK(check_);
  CHECK(event_);
  CHECK(changed_);
  CHECK(request_timeout_.is_positive());
}

ElectronPermissionPolicy::~ElectronPermissionPolicy() {
  for (auto& [id, pending] : pending_) {
    std::move(pending->callback).Run(base::Value());
  }
}

base::expected<void, std::string> ElectronPermissionPolicy::SetHandler(
    const std::string& partition,
    const std::string& kind,
    int registration,
    bool enabled) {
  scoped_refptr keep_alive(this);
  if (closed_) {
    return base::unexpected("ERR_ABORTED: permission owner was retired");
  }
  if ((kind != "check" && kind != "request" && kind != "display") ||
      registration <= 0) {
    return base::unexpected(
        "ERR_INVALID_ARG_VALUE: invalid permission handler");
  }
  const auto key = std::make_pair(partition, kind);
  auto& previous = registrations_[key];
  if (registration <= previous.id) {
    return base::unexpected("ERR_INVALID_STATE: stale permission registration");
  }
  previous = {registration, enabled};
  if (kind != "check") {
    CancelPartition(partition, kind);
  }
  changed_.Run();
  return {};
}

base::expected<bool, std::string> ElectronPermissionPolicy::Check(
    const std::string& partition,
    base::DictValue request,
    ValidCallback valid,
    bool default_granted) {
  scoped_refptr keep_alive(this);
  if (closed_ || !valid.Run()) {
    return false;
  }
  const auto key = std::make_pair(partition, "check");
  const Registration registration = registrations_[key];
  if (!registration.enabled) {
    return default_granted;
  }
  request.Set("partition", partition);
  request.Set("registration", registration.id);
  const auto result = check_.Run(base::Value(std::move(request)));
  if (closed_ || !valid.Run() || registrations_[key].id != registration.id) {
    return false;
  }
  return result;
}

void ElectronPermissionPolicy::Request(
    const std::string& partition,
    base::DictValue request,
    ValidCallback valid,
    base::OnceCallback<void(bool)> callback) {
  RequestValue(
      partition, "request", std::move(request), std::move(valid),
      base::BindOnce(
          [](base::OnceCallback<void(bool)> callback, base::Value result) {
            std::move(callback).Run(result.is_bool() && result.GetBool());
          },
          std::move(callback)));
}

void ElectronPermissionPolicy::RequestDisplay(
    const std::string& partition,
    base::DictValue request,
    ValidCallback valid,
    base::OnceCallback<void(base::DictValue)> callback) {
  RequestValue(partition, "display", std::move(request), std::move(valid),
               base::BindOnce(
                   [](base::OnceCallback<void(base::DictValue)> callback,
                      base::Value result) {
                     std::move(callback).Run(result.is_dict()
                                                 ? std::move(result).TakeDict()
                                                 : base::DictValue());
                   },
                   std::move(callback)));
}

void ElectronPermissionPolicy::RequestValue(
    const std::string& partition,
    const std::string& kind,
    base::DictValue request,
    ValidCallback valid,
    base::OnceCallback<void(base::Value)> callback) {
  scoped_refptr keep_alive(this);
  if (closed_ || !valid.Run()) {
    std::move(callback).Run(base::Value());
    return;
  }
  const Registration registration = registrations_[{partition, kind}];
  if (!registration.enabled) {
    std::move(callback).Run(base::Value(kind == "request"));
    return;
  }
  if (next_request_id_ == std::numeric_limits<int>::max()) {
    std::move(callback).Run(base::Value());
    return;
  }
  const int id = next_request_id_++;
  auto pending = std::make_unique<Pending>();
  pending->partition = partition;
  pending->kind = kind;
  pending->registration = registration.id;
  pending->valid = std::move(valid);
  pending->callback = std::move(callback);
  pending->deadline.Start(
      FROM_HERE, request_timeout_,
      base::BindOnce(&ElectronPermissionPolicy::Finish,
                     weak_factory_.GetWeakPtr(), id, base::Value()));
  pending_.emplace(id, std::move(pending));
  request.Set("partition", partition);
  request.Set("registration", registration.id);
  request.Set("requestId", id);
  request.Set("kind", kind);
  event_.Run(base::Value(std::move(request)));
}

bool ElectronPermissionPolicy::Respond(const std::string& partition,
                                       int registration,
                                       int request_id,
                                       bool granted) {
  return RespondValue(partition, "request", registration, request_id,
                      base::Value(granted));
}

bool ElectronPermissionPolicy::RespondDisplay(const std::string& partition,
                                              int registration,
                                              int request_id,
                                              base::DictValue selection) {
  return RespondValue(partition, "display", registration, request_id,
                      base::Value(std::move(selection)));
}

bool ElectronPermissionPolicy::RespondValue(const std::string& partition,
                                            const std::string& kind,
                                            int registration,
                                            int request_id,
                                            base::Value value) {
  scoped_refptr keep_alive(this);
  auto found = pending_.find(request_id);
  if (found == pending_.end() || found->second->partition != partition ||
      found->second->kind != kind ||
      found->second->registration != registration) {
    return false;
  }
  Finish(request_id, std::move(value));
  return true;
}

void ElectronPermissionPolicy::Finish(int request_id, base::Value value) {
  scoped_refptr keep_alive(this);
  auto found = pending_.find(request_id);
  if (found == pending_.end()) {
    return;
  }
  auto pending = std::move(found->second);
  pending_.erase(found);
  pending->deadline.Stop();
  if (closed_ || !pending->valid.Run()) {
    value = base::Value();
  }
  std::move(pending->callback).Run(std::move(value));
  changed_.Run();
}

void ElectronPermissionPolicy::CancelPartition(const std::string& partition,
                                               const std::string& kind) {
  std::vector<int> ids;
  for (const auto& [id, pending] : pending_) {
    if (pending->partition == partition && pending->kind == kind) {
      ids.push_back(id);
    }
  }
  for (const int id : ids) {
    Finish(id, base::Value());
  }
}

void ElectronPermissionPolicy::Close() {
  scoped_refptr keep_alive(this);
  if (closed_) {
    return;
  }
  closed_ = true;
  while (!pending_.empty()) {
    Finish(pending_.begin()->first, base::Value());
  }
  registrations_.clear();
  changed_.Run();
}

}  // namespace xenon::ipc

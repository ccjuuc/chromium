// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_check_remote.h"

#include <atomic>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/threading/sequence_local_storage_slot.h"
#include "base/timer/timer.h"
#include "mojo/public/cpp/bindings/sync_call_restrictions.h"

namespace xenon::ipc {
namespace {

size_t& PermissionCheckDepth() {
  static base::SequenceLocalStorageSlot<size_t> depth;
  return depth.GetOrCreateValue();
}

class ScopedPermissionCheck {
 public:
  ScopedPermissionCheck() { ++PermissionCheckDepth(); }
  ScopedPermissionCheck(const ScopedPermissionCheck&) = delete;
  ScopedPermissionCheck& operator=(const ScopedPermissionCheck&) = delete;
  ~ScopedPermissionCheck() {
    CHECK_GT(PermissionCheckDepth(), 0u);
    // Creating other sequence-local slots during a reverse call can relocate
    // the storage. Reacquire the value rather than retaining its address.
    --PermissionCheckDepth();
  }
};

// A reply and its deadline race through one atomic decision. The deadline never
// depends on a posted task on the calling sequence, which may be in SyncWatch.
class CheckDeadline final
    : public base::RefCountedDeleteOnSequence<CheckDeadline> {
 public:
  CheckDeadline(scoped_refptr<base::SequencedTaskRunner> runner,
                mojo::SharedRemote<mojom::ElectronPermissionCheck> remote,
                base::TimeDelta timeout);
  void Arm();
  bool Finish();
  void Stop();

 private:
  friend class base::RefCountedDeleteOnSequence<CheckDeadline>;
  friend class base::DeleteHelper<CheckDeadline>;
  enum class State { kPending, kCompleted, kTimedOut };
  ~CheckDeadline();
  void OnTimeout();

  std::atomic<State> state_{State::kPending};
  mojo::SharedRemote<mojom::ElectronPermissionCheck> remote_;
  const base::TimeDelta timeout_;
  base::OneShotTimer timer_;
};

CheckDeadline::CheckDeadline(
    scoped_refptr<base::SequencedTaskRunner> runner,
    mojo::SharedRemote<mojom::ElectronPermissionCheck> remote,
    base::TimeDelta timeout)
    : base::RefCountedDeleteOnSequence<CheckDeadline>(std::move(runner)),
      remote_(std::move(remote)),
      timeout_(timeout) {}

CheckDeadline::~CheckDeadline() = default;

void CheckDeadline::Arm() {
  if (state_.load() == State::kPending) {
    timer_.Start(
        FROM_HERE, timeout_,
        base::BindOnce(&CheckDeadline::OnTimeout, base::Unretained(this)));
  }
}

bool CheckDeadline::Finish() {
  State expected = State::kPending;
  return state_.compare_exchange_strong(expected, State::kCompleted);
}

void CheckDeadline::Stop() {
  timer_.Stop();
}

void CheckDeadline::OnTimeout() {
  State expected = State::kPending;
  if (state_.compare_exchange_strong(expected, State::kTimedOut)) {
    remote_.Disconnect();
  }
}

}  // namespace

bool IsElectronPermissionCheckInProgress() {
  return PermissionCheckDepth() != 0;
}

ElectronPermissionCheckRemote::ElectronPermissionCheckRemote(
    mojo::PendingRemote<mojom::ElectronPermissionCheck> remote,
    scoped_refptr<base::SequencedTaskRunner> transport_runner,
    base::TimeDelta timeout)
    : transport_runner_(std::move(transport_runner)), timeout_(timeout) {
  CHECK(transport_runner_);
  CHECK(timeout_.is_positive());
  if (remote.is_valid()) {
    remote_.Bind(std::move(remote), transport_runner_);
  }
}

ElectronPermissionCheckRemote::~ElectronPermissionCheckRemote() {
  Close();
}

base::expected<bool, std::string> ElectronPermissionCheckRemote::Check(
    base::Value request) {
  // A reverse synchronous call can retire the owner while this method waits.
  // Retain only local state from this point; do not access |this| after
  // Check().
  auto remote = remote_;
  auto runner = transport_runner_;
  auto closed = closed_;
  if (!remote.is_bound() || closed->data.load()) {
    return base::unexpected("ERR_ABORTED: permission transport is closed");
  }
  auto deadline = base::MakeRefCounted<CheckDeadline>(runner, remote, timeout_);
  if (runner->RunsTasksInCurrentSequence()) {
    return base::unexpected(
        "ERR_INVALID_STATE: permission deadline requires another sequence");
  }
  if (!runner->PostTask(FROM_HERE,
                        base::BindOnce(&CheckDeadline::Arm, deadline))) {
    return base::unexpected("ERR_ABORTED: permission transport stopped");
  }
  bool granted = false;
  std::string error;
  bool replied = false;
  {
    ScopedPermissionCheck checking;
#if BUILDFLAG(ENABLE_XENON_SERVICE)
    mojo::SyncCallRestrictions::ScopedAllowSyncCall allow;
    mojo::SyncCallRestrictions::ScopedAllowSyncCallInterrupts allow_getters;
#endif
    replied = remote->Check(std::move(request), &granted, &error);
  }
  const bool in_time = deadline->Finish();
  runner->PostTask(FROM_HERE,
                   base::BindOnce(&CheckDeadline::Stop, std::move(deadline)));
  if (closed->data.load()) {
    return base::unexpected("ERR_ABORTED: permission owner was retired");
  }
  if (!in_time) {
    return base::unexpected("ERR_TIMED_OUT: permission check deadline expired");
  }
  if (!replied) {
    return base::unexpected("ERR_ABORTED: permission check disconnected");
  }
  if (!error.empty()) {
    return base::unexpected(std::move(error));
  }
  return granted;
}

void ElectronPermissionCheckRemote::Close() {
  closed_->data.store(true);
  if (remote_.is_bound()) {
    remote_.Disconnect();
  }
}

}  // namespace xenon::ipc

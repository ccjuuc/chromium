// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_XENON_TRANSFER_SERVICE_H_
#define XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_XENON_TRANSFER_SERVICE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "xenon_overlay/chrome/browser/account/xenon_account_service.h"

class Profile;
namespace cross_transfer {
class TestTransferJob;
struct TestJobOptions;
}  // namespace cross_transfer

// Own-device invitations and file jobs live with the profile, not the page.
// Relay credentials and source paths never form part of the public state.
class XenonTransferService final : public base::SupportsUserData::Data {
 public:
  using StateCallback = base::OnceCallback<void(base::DictValue)>;
  static XenonTransferService* GetForProfile(Profile* profile);
  ~XenonTransferService() override;

  base::DictValue GetState() const;
  void Refresh(StateCallback callback);
  void SendText(std::string receiver_id,
                std::string text,
                StateCallback callback);
  void SendFile(std::string receiver_id,
                base::FilePath source,
                StateCallback callback);
  void Respond(std::string invitation_id,
               std::string action,
               StateCallback callback);
  void Cancel(StateCallback callback);

 private:
  friend class XenonTransferServiceTestPeer;

  using Response = XenonAccountService::Response;
  struct SourceFile {
    base::FilePath path;
    std::string name;
    uint64_t size = 0;
    std::string error;
  };
  struct PendingFinalization {
    std::string id;
    bool success;
    base::Time expires_at;
    std::string error;
  };

  explicit XenonTransferService(Profile* profile);
  void Initialize();
  void OnSessionChanged();
  bool Ready(StateCallback& callback);
  bool IsCurrent(uint64_t generation, StateCallback& callback);
  void Finish(StateCallback callback, std::string error = {});
  void OnListed(uint64_t generation,
                uint64_t revision,
                StateCallback callback,
                Response reply);
  void CreateInvitation(std::string receiver_id,
                        std::string text,
                        SourceFile source,
                        StateCallback callback);
  void OnFileInspected(uint64_t generation,
                       std::string receiver_id,
                       StateCallback callback,
                       SourceFile source);
  void OnCreated(uint64_t generation,
                 SourceFile source,
                 StateCallback callback,
                 Response reply);
  void OnResponded(uint64_t generation,
                   std::string invitation_id,
                   std::string action,
                   StateCallback callback,
                   Response reply);
  void ConsiderStartingFile();
  void RequestRelay(const base::DictValue& invitation, bool sending);
  void OnRelayReady(uint64_t generation,
                    base::DictValue invitation,
                    bool sending,
                    Response reply);
  void OnJobFinished(uint64_t generation, std::string invitation_id);
  void QueueFinalization(std::string invitation_id, bool success);
  void FinalizeFile();
  void OnFinalized(uint64_t generation,
                   std::string invitation_id,
                   Response reply);
  void ClearFinalization();
  void StopLocalFile();
  void ForgetFile(const std::string& invitation_id);
  const base::DictValue* FindInvitation(const std::string& id) const;
  bool HasFileWork() const;

  const raw_ptr<Profile> profile_;
  std::string account_id_;
  std::string device_id_;
  uint64_t generation_ = 0;
  uint64_t list_revision_ = 0;
  bool polling_ = false;
  bool operation_busy_ = false;
  bool relay_pending_ = false;
  bool job_running_ = false;
  std::string active_id_;
  std::string active_role_;
  std::string active_name_;
  std::string error_;
  base::ListValue invitations_;
  std::map<std::string, SourceFile> source_files_;
  std::set<std::string> accepted_receives_;
  std::set<std::string> attempted_files_;
  std::shared_ptr<cross_transfer::TestTransferJob> job_;
  std::optional<PendingFinalization> pending_finalization_;
  bool finalization_in_flight_ = false;
  base::CallbackListSubscription session_subscription_;
  base::RepeatingTimer poll_timer_;
  base::OneShotTimer finalization_retry_timer_;
  base::WeakPtrFactory<XenonTransferService> weak_factory_{this};
};

#endif  // XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_XENON_TRANSFER_SERVICE_H_

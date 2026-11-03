// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_XENON_TRANSFER_SERVICE_H_
#define XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_XENON_TRANSFER_SERVICE_H_

#include <cstdint>
#include <map>
#include <memory>
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
namespace base {
class ScopedTempDir;
}
namespace cross_transfer {
class TestTransferJob;
}
namespace xenon {
class WebRtcConnectionCache;
}

// The profile owns task state and history. The network carries peer messages;
// delivery acknowledgements never determine whether a payload was transferred.
class XenonTransferService final : public base::SupportsUserData::Data {
 public:
  using StateCallback = base::OnceCallback<void(base::DictValue)>;
  static XenonTransferService* GetForProfile(Profile* profile);
  ~XenonTransferService() override;
  base::DictValue GetState() const;
  void Refresh(StateCallback callback, bool retry_signals = false);
  void SetUseStun(bool enabled, StateCallback callback);
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
    std::string text;
    std::shared_ptr<base::ScopedTempDir> temporary_directory;
  };
  struct PreparedJob {
    std::shared_ptr<cross_transfer::TestTransferJob> job;
    std::shared_ptr<base::ScopedTempDir> temporary_directory;
    base::OnceClosure release_channel;
  };
  struct JobResult {
    bool success = false;
    std::string text;
  };
  explicit XenonTransferService(Profile* profile);
  void Initialize();
  void ResetDirectConnections();
  void OnSessionChanged();
  bool Ready(StateCallback& callback);
  bool IsCurrent(uint64_t generation, StateCallback& callback);
  void Finish(StateCallback callback, std::string error = {});
  void LoadLocalTasks();
  void PersistLocalTasks();
  void ExpireLocalTasks();
  bool WasDirectSuccessful(const std::string& peer) const;
  void RememberDirectSuccess(const std::string& peer);
  void SetTaskState(const std::string& id,
                    std::string status,
                    std::string error = {});
  void FailTask(const std::string& id,
                std::string error,
                bool notify_peer = true);
  void ApplySignal(const base::DictValue& signal);
  void QueueSignal(const std::string& id, std::string type);
  void PumpSignalOutbox(bool retry_now = false);
  void OnOutboxPersisted(uint64_t generation, uint64_t revision);
  void HandleJobFailure(const std::string& id, bool completion_requested);
  void OnSignalPublished(uint64_t generation,
                         std::string message_id,
                         Response reply);
  void OnSignals(uint64_t generation, StateCallback callback, Response reply);
  void AcknowledgeSignals(uint64_t generation,
                          base::ListValue message_ids,
                          StateCallback callback);
  void CreateInvitation(std::string receiver_id,
                        SourceFile source,
                        StateCallback callback);
  void OnFileInspected(uint64_t generation,
                       std::string receiver_id,
                       StateCallback callback,
                       SourceFile source);
  void SetActiveFile(const base::DictValue& invitation);
  void ConsiderStartingFile();
  void StartJob(const base::DictValue& invitation, bool sending);
  void OnJobPrepared(uint64_t generation,
                     std::string invitation_id,
                     bool sending,
                     bool text_payload,
                     PreparedJob prepared);
  void OnJobFinished(uint64_t generation,
                     std::string invitation_id,
                     JobResult result);
  void SaveText(const std::string& id, const std::string& text);
  void RecordCompletedTransfer(const std::string& invitation_id);
  void StopLocalFile();
  void ForgetFile(const std::string& invitation_id);
  const base::DictValue* FindInvitation(const std::string& id) const;
  bool HasFileWork() const;

  const raw_ptr<Profile> profile_;
  std::shared_ptr<xenon::WebRtcConnectionCache> direct_connections_;
  std::string account_id_;
  std::string device_id_;
  uint64_t generation_ = 0;
  bool polling_ = false;
  bool operation_busy_ = false;
  bool relay_pending_ = false;
  bool job_running_ = false;
  std::string active_id_;
  std::string active_role_;
  std::string active_name_;
  std::string file_error_;
  bool active_text_ = false;
  base::DictValue active_invitation_;
  base::DictValue local_tasks_;
  base::DictValue direct_peers_;
  base::DictValue signal_outbox_;
  base::DictValue seen_signals_;
  base::DictValue cancelled_before_offer_;
  std::string signal_in_flight_id_;
  uint64_t outbox_write_revision_ = 0;
  bool outbox_write_pending_ = false;
  std::string error_;
  std::map<std::string, SourceFile> source_files_;
  std::set<std::string> accepted_receives_;
  std::set<std::string> file_attempts_;
  std::shared_ptr<cross_transfer::TestTransferJob> job_;
  base::CallbackListSubscription session_subscription_;
  base::RepeatingTimer poll_timer_;
  base::OneShotTimer signal_retry_timer_;
  base::WeakPtrFactory<XenonTransferService> weak_factory_{this};
};
#endif

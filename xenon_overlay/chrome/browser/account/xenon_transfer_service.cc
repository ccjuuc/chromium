// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/account/xenon_transfer_service.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "cross_transfer/test_job.h"
#include "cross_transfer/tls_transport_channel.h"

namespace {

constexpr char kUserDataKey[] = "xenon.own_device_transfer_service";
constexpr uint64_t kMaxFileSize = uint64_t{8} * 1024 * 1024 * 1024;
constexpr size_t kMaxTextBytes = 16 * 1024;

bool IsId(const std::string& value) {
  return value.size() == 32 && std::ranges::all_of(value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool IsTerminal(const std::string& value) {
  return value == "completed" || value == "rejected" || value == "cancelled" ||
         value == "expired" || value == "failed";
}

std::string ApiError(const XenonAccountService::Response& reply) {
  if (!reply.error.empty()) {
    return reply.error;
  }
  if (reply.status == 401) {
    return "登录已失效，请重新登录";
  }
  if (reply.status == 403 || reply.status == 404) {
    return "设备或传输请求已不可用";
  }
  if (reply.status == 409) {
    return "传输状态已变化，请刷新后重试";
  }
  if (reply.status == 429) {
    return "请求过多或传输数量已达上限，请稍后重试";
  }
  return "无法完成传输请求，请稍后重试";
}

std::optional<base::DictValue> SanitizeInvitation(
    const base::DictValue& input,
    const std::string& device_id) {
  const auto* id = input.FindString("id");
  const auto* sender = input.FindString("senderDeviceId");
  const auto* receiver = input.FindString("receiverDeviceId");
  const auto* sender_name = input.FindString("senderName");
  const auto* receiver_name = input.FindString("receiverName");
  const auto* kind = input.FindString("kind");
  const auto* status = input.FindString("status");
  const auto* text = input.FindString("text");
  const auto* name = input.FindString("fileName");
  const auto size = input.FindDouble("fileSize");
  const auto created = input.FindDouble("createdAt");
  const auto expires = input.FindDouble("expiresAt");
  if (!id || !IsId(*id) || !sender || !IsId(*sender) || !receiver ||
      !IsId(*receiver) || *sender == *receiver ||
      (*sender != device_id && *receiver != device_id) || !sender_name ||
      sender_name->size() > 512 || !receiver_name ||
      receiver_name->size() > 512 || !kind ||
      (*kind != "text" && *kind != "file") || !status ||
      (*status != "pending" && *status != "accepted" && !IsTerminal(*status)) ||
      !text || text->size() > kMaxTextBytes || !base::IsStringUTF8(*text) ||
      !name || name->size() > 1024 || !base::IsStringUTF8(*name) || !size ||
      !std::isfinite(*size) || *size < 0 || std::floor(*size) != *size ||
      *size > kMaxFileSize || !created || !std::isfinite(*created) ||
      !expires || !std::isfinite(*expires)) {
    return std::nullopt;
  }
  if (*kind == "file") {
    const auto path = base::FilePath::FromUTF8Unsafe(*name);
    if (name->empty() || path.BaseName() != path || path.ReferencesParent() ||
        name->find_first_of("/\\:\0", 0, 4) != std::string::npos ||
        !text->empty()) {
      return std::nullopt;
    }
  } else if (text->empty() || !name->empty() || *size != 0) {
    return std::nullopt;
  }
  // Build a display-only object. Never forward future server credential fields.
  return base::DictValue()
      .Set("id", *id)
      .Set("senderDeviceId", *sender)
      .Set("receiverDeviceId", *receiver)
      .Set("senderName", *sender_name)
      .Set("receiverName", *receiver_name)
      .Set("direction", *receiver == device_id ? "incoming" : "outgoing")
      .Set("kind", *kind)
      .Set("text", *text)
      .Set("fileName", *name)
      .Set("fileSize", *size)
      .Set("status", *status)
      .Set("createdAt", *created)
      .Set("expiresAt", *expires);
}

}  // namespace

// static
XenonTransferService* XenonTransferService::GetForProfile(Profile* profile) {
  profile = profile->GetOriginalProfile();
  auto* service =
      static_cast<XenonTransferService*>(profile->GetUserData(kUserDataKey));
  if (!service) {
    auto owned = std::unique_ptr<XenonTransferService>(
        new XenonTransferService(profile));
    service = owned.get();
    profile->SetUserData(kUserDataKey, std::move(owned));
    service->Initialize();
  }
  return service;
}

XenonTransferService::XenonTransferService(Profile* profile)
    : profile_(profile) {}

XenonTransferService::~XenonTransferService() {
  StopLocalFile();
}

void XenonTransferService::Initialize() {
  session_subscription_ =
      XenonAccountService::GetForProfile(profile_)->AddSessionChangedCallback(
          base::BindRepeating(&XenonTransferService::OnSessionChanged,
                              weak_factory_.GetWeakPtr()));
  OnSessionChanged();
  poll_timer_.Start(FROM_HERE, base::Seconds(2),
                    base::BindRepeating(
                        [](base::WeakPtr<XenonTransferService> service) {
                          if (service) {
                            service->Refresh({});
                          }
                        },
                        weak_factory_.GetWeakPtr()));
}

void XenonTransferService::OnSessionChanged() {
  auto* account = XenonAccountService::GetForProfile(profile_);
  const auto account_id = account->GetAccountId();
  const auto device_id = account->GetDeviceId();
  if (account_id == account_id_ && device_id == device_id_) {
    return;
  }
  ++generation_;
  ++list_revision_;
  StopLocalFile();
  ClearFinalization();
  account_id_ = account_id;
  device_id_ = device_id;
  polling_ = false;
  operation_busy_ = false;
  relay_pending_ = false;
  job_running_ = false;
  job_.reset();
  active_id_.clear();
  active_role_.clear();
  active_name_.clear();
  source_files_.clear();
  accepted_receives_.clear();
  attempted_files_.clear();
  invitations_.clear();
  error_.clear();
}

base::DictValue XenonTransferService::GetState() const {
  base::DictValue job;
  job.Set("id", active_id_);
  job.Set("transferId", active_id_);
  job.Set("role", active_role_);
  job.Set("fileName", active_name_);
  job.Set("active", HasFileWork());
  job.Set("state", "idle");
  job.Set("status", "");
  job.Set("error", "");
  job.Set("savedPath", "");
  job.Set("transferredBytes", 0.0);
  job.Set("totalBytes", 0.0);
  job.Set("percent", 0.0);
  if (job_) {
    const auto snapshot = job_->snapshot();
    job.Set("state", snapshot.state);
    job.Set("status", snapshot.status);
    job.Set("error", snapshot.error);
    job.Set("savedPath", snapshot.saved_path);
    job.Set("transferredBytes", static_cast<double>(snapshot.bytes));
    job.Set("totalBytes", static_cast<double>(snapshot.total));
    job.Set("percent", snapshot.percent);
  } else if (relay_pending_) {
    job.Set("state", "connecting");
    job.Set("status", "正在建立传输连接…");
  } else if (!source_files_.empty()) {
    job.Set("id", source_files_.begin()->first);
    job.Set("transferId", source_files_.begin()->first);
    job.Set("role", "send");
    job.Set("fileName", source_files_.begin()->second.name);
    job.Set("state", "waiting");
    job.Set("status", "等待对方接收");
    job.Set("totalBytes",
            static_cast<double>(source_files_.begin()->second.size));
  }
  if (pending_finalization_ && pending_finalization_->success) {
    job.Set("status", "文件已保存，正在确认完成…");
  }
  return base::DictValue()
      .Set("transfers", invitations_.Clone())
      .Set("job", std::move(job))
      .Set("busy", operation_busy_)
      .Set("error",
           pending_finalization_ && !pending_finalization_->error.empty()
               ? pending_finalization_->error
               : error_);
}

bool XenonTransferService::Ready(StateCallback& callback) {
  OnSessionChanged();
  if (!account_id_.empty() && !device_id_.empty() &&
      XenonAccountService::GetForProfile(profile_)->HasAuthenticatedSession()) {
    return true;
  }
  Finish(std::move(callback), "请先登录并等待本机设备登记完成");
  return false;
}

bool XenonTransferService::IsCurrent(uint64_t generation,
                                     StateCallback& callback) {
  OnSessionChanged();
  if (generation == generation_ && !device_id_.empty()) {
    return true;
  }
  if (callback) {
    std::move(callback).Run(GetState());
  }
  return false;
}

void XenonTransferService::Finish(StateCallback callback, std::string error) {
  operation_busy_ = false;
  error_ = std::move(error);
  if (callback) {
    std::move(callback).Run(GetState());
  }
}

void XenonTransferService::Refresh(StateCallback callback) {
  OnSessionChanged();
  if (polling_ || operation_busy_ || account_id_.empty() ||
      device_id_.empty()) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  polling_ = true;
  XenonAccountService::GetForProfile(profile_)->RequestForSession(
      "GET", "/devices/" + device_id_ + "/transfers", {},
      base::BindOnce(&XenonTransferService::OnListed,
                     weak_factory_.GetWeakPtr(), generation_, list_revision_,
                     std::move(callback)));
}

void XenonTransferService::OnListed(uint64_t generation,
                                    uint64_t revision,
                                    StateCallback callback,
                                    Response reply) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  polling_ = false;
  if (revision != list_revision_) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  if (reply.status != 200) {
    error_ = ApiError(reply);
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  const auto* items = reply.body.FindList("transfers");
  if (!items || items->size() > 32) {
    error_ = "服务端返回了无效传输列表";
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  base::ListValue sanitized;
  for (const auto& item : *items) {
    auto value = item.is_dict() ? SanitizeInvitation(item.GetDict(), device_id_)
                                : std::nullopt;
    if (!value) {
      error_ = "服务端返回了无效传输请求";
      if (callback) {
        std::move(callback).Run(GetState());
      }
      return;
    }
    sanitized.Append(std::move(*value));
  }
  invitations_ = std::move(sanitized);
  error_.clear();
  const auto no_longer_pending = [&](const std::string& id) {
    const auto* invitation = FindInvitation(id);
    return !invitation || IsTerminal(*invitation->FindString("status"));
  };
  if (pending_finalization_ && no_longer_pending(pending_finalization_->id)) {
    ForgetFile(pending_finalization_->id);
    ClearFinalization();
  }
  std::erase_if(attempted_files_, no_longer_pending);
  std::erase_if(accepted_receives_, no_longer_pending);
  for (auto it = source_files_.begin(); it != source_files_.end();) {
    const auto* invitation = FindInvitation(it->first);
    if (!invitation || IsTerminal(*invitation->FindString("status"))) {
      it = source_files_.erase(it);
    } else {
      ++it;
    }
  }
  if (!active_id_.empty()) {
    const auto* invitation = FindInvitation(active_id_);
    if (!invitation || (IsTerminal(*invitation->FindString("status")) &&
                        *invitation->FindString("status") != "completed")) {
      StopLocalFile();
    }
  }
  ConsiderStartingFile();
  if (callback) {
    std::move(callback).Run(GetState());
  }
}

void XenonTransferService::SendText(std::string receiver_id,
                                    std::string text,
                                    StateCallback callback) {
  if (!Ready(callback)) {
    return;
  }
  if (operation_busy_) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  if (!IsId(receiver_id) || receiver_id == device_id_ || text.empty() ||
      text.size() > kMaxTextBytes || text.find('\0') != std::string::npos ||
      !base::IsStringUTF8(text)) {
    Finish(std::move(callback), "请选择其他设备并输入不超过 16 KB 的文本");
    return;
  }
  operation_busy_ = true;
  ++list_revision_;
  CreateInvitation(std::move(receiver_id), std::move(text), {},
                   std::move(callback));
}

void XenonTransferService::SendFile(std::string receiver_id,
                                    base::FilePath source,
                                    StateCallback callback) {
  if (!Ready(callback)) {
    return;
  }
  if (operation_busy_) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  if (HasFileWork() || !IsId(receiver_id) || receiver_id == device_id_ ||
      !source.IsAbsolute()) {
    Finish(std::move(callback), "请选择其他设备和文件，并等待当前文件传输结束");
    return;
  }
  operation_busy_ = true;
  ++list_revision_;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](base::FilePath path) {
            SourceFile file;
            file.path = std::move(path);
            file.name = file.path.BaseName().AsUTF8Unsafe();
            base::File::Info info;
            if (!base::GetFileInfo(file.path, &info) || info.is_directory ||
                info.is_symbolic_link || info.size < 0 ||
                static_cast<uint64_t>(info.size) > kMaxFileSize ||
                file.name.empty() || !base::IsStringUTF8(file.name)) {
              file.error = "无法读取文件；请选择不超过 8 GB 的普通文件";
            } else {
              file.size = static_cast<uint64_t>(info.size);
            }
            return file;
          },
          std::move(source)),
      base::BindOnce(&XenonTransferService::OnFileInspected,
                     weak_factory_.GetWeakPtr(), generation_,
                     std::move(receiver_id), std::move(callback)));
}

void XenonTransferService::OnFileInspected(uint64_t generation,
                                           std::string receiver_id,
                                           StateCallback callback,
                                           SourceFile source) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  if (!source.error.empty()) {
    Finish(std::move(callback), source.error);
    return;
  }
  CreateInvitation(std::move(receiver_id), {}, std::move(source),
                   std::move(callback));
}

void XenonTransferService::CreateInvitation(std::string receiver_id,
                                            std::string text,
                                            SourceFile source,
                                            StateCallback callback) {
  base::DictValue body;
  body.Set("senderDeviceId", device_id_);
  body.Set("receiverDeviceId", receiver_id);
  body.Set("kind", source.path.empty() ? "text" : "file");
  body.Set("text", std::move(text));
  body.Set("fileName", source.name);
  body.Set("fileSize", static_cast<double>(source.size));
  XenonAccountService::GetForProfile(profile_)->RequestForSession(
      "POST", "/transfers", std::move(body),
      base::BindOnce(&XenonTransferService::OnCreated,
                     weak_factory_.GetWeakPtr(), generation_, std::move(source),
                     std::move(callback)));
}

void XenonTransferService::OnCreated(uint64_t generation,
                                     SourceFile source,
                                     StateCallback callback,
                                     Response reply) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  const auto* input = reply.body.FindDict("transfer");
  auto invitation =
      input ? SanitizeInvitation(*input, device_id_) : std::nullopt;
  if ((reply.status != 200 && reply.status != 201) || !invitation) {
    Finish(std::move(callback), ApiError(reply));
    return;
  }
  if (*invitation->FindString("senderDeviceId") != device_id_) {
    Finish(std::move(callback), "服务端返回了无效传输请求");
    return;
  }
  if (!source.path.empty()) {
    const auto id = *invitation->FindString("id");
    source_files_.emplace(id, std::move(source));
    job_.reset();
    active_id_.clear();
  }
  invitations_.Append(std::move(*invitation));
  Finish(std::move(callback));
}

void XenonTransferService::Respond(std::string invitation_id,
                                   std::string action,
                                   StateCallback callback) {
  if (!Ready(callback)) {
    return;
  }
  if (operation_busy_) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  const auto* invitation = FindInvitation(invitation_id);
  if (pending_finalization_ && pending_finalization_->id == invitation_id) {
    Finish(std::move(callback), "正在确认文件传输结果，请稍后重试");
    return;
  }
  const bool accepting_file = invitation && action == "accept" &&
                              *invitation->FindString("kind") == "file";
  if (!IsId(invitation_id) || !invitation ||
      (action != "accept" && action != "reject" && action != "cancel") ||
      (action != "cancel" &&
       *invitation->FindString("receiverDeviceId") != device_id_) ||
      (accepting_file && HasFileWork() && active_id_ != invitation_id)) {
    Finish(std::move(callback), "传输状态已变化，或另一个文件正在传输");
    return;
  }
  operation_busy_ = true;
  ++list_revision_;
  if (action == "cancel") {
    if (active_id_ == invitation_id) {
      StopLocalFile();
    }
    ForgetFile(invitation_id);
  }
  XenonAccountService::GetForProfile(profile_)->RequestForSession(
      "POST", "/transfers/" + invitation_id + "/" + action,
      base::DictValue().Set("deviceId", device_id_),
      base::BindOnce(&XenonTransferService::OnResponded,
                     weak_factory_.GetWeakPtr(), generation_, invitation_id,
                     action, std::move(callback)));
}

void XenonTransferService::OnResponded(uint64_t generation,
                                       std::string invitation_id,
                                       std::string action,
                                       StateCallback callback,
                                       Response reply) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  if (reply.status != 200) {
    Finish(std::move(callback), ApiError(reply));
    return;
  }
  const auto* input = reply.body.FindDict("transfer");
  auto invitation =
      input ? SanitizeInvitation(*input, device_id_) : std::nullopt;
  if (!invitation || *invitation->FindString("id") != invitation_id) {
    Finish(std::move(callback), "服务端返回了无效传输状态");
    return;
  }
  for (auto& item : invitations_) {
    if (*item.GetDict().FindString("id") == invitation_id) {
      item = base::Value(invitation->Clone());
      break;
    }
  }
  if (action == "accept" && *invitation->FindString("kind") == "file") {
    accepted_receives_.insert(invitation_id);
    attempted_files_.erase(invitation_id);
  } else if (action == "reject" || action == "cancel") {
    ForgetFile(invitation_id);
  }
  ConsiderStartingFile();
  Finish(std::move(callback));
}

void XenonTransferService::Cancel(StateCallback callback) {
  const std::string id = !active_id_.empty()     ? active_id_
                         : source_files_.empty() ? ""
                                                 : source_files_.begin()->first;
  if (id.empty()) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  Respond(id, "cancel", std::move(callback));
}

const base::DictValue* XenonTransferService::FindInvitation(
    const std::string& id) const {
  for (const auto& item : invitations_) {
    if (*item.GetDict().FindString("id") == id) {
      return &item.GetDict();
    }
  }
  return nullptr;
}

bool XenonTransferService::HasFileWork() const {
  return job_running_ || relay_pending_ || !source_files_.empty() ||
         pending_finalization_.has_value();
}

void XenonTransferService::ConsiderStartingFile() {
  if (job_running_ || relay_pending_ || pending_finalization_) {
    return;
  }
  for (const auto& item : invitations_) {
    const auto& invitation = item.GetDict();
    const auto& id = *invitation.FindString("id");
    if (*invitation.FindString("kind") != "file" ||
        *invitation.FindString("status") != "accepted" ||
        attempted_files_.contains(id)) {
      continue;
    }
    const bool sending = *invitation.FindString("senderDeviceId") == device_id_;
    if ((sending && source_files_.contains(id)) ||
        (!sending && accepted_receives_.contains(id))) {
      RequestRelay(invitation, sending);
      return;
    }
  }
}

void XenonTransferService::RequestRelay(const base::DictValue& invitation,
                                        bool sending) {
  active_id_ = *invitation.FindString("id");
  active_role_ = sending ? "send" : "receive";
  active_name_ = *invitation.FindString("fileName");
  relay_pending_ = true;
  attempted_files_.insert(active_id_);
  job_.reset();
  XenonAccountService::GetForProfile(profile_)->RequestForSession(
      "POST", "/transfers/" + active_id_ + "/relay",
      base::DictValue().Set("deviceId", device_id_),
      base::BindOnce(&XenonTransferService::OnRelayReady,
                     weak_factory_.GetWeakPtr(), generation_,
                     invitation.Clone(), sending));
}

void XenonTransferService::OnRelayReady(uint64_t generation,
                                        base::DictValue invitation,
                                        bool sending,
                                        Response reply) {
  StateCallback unused;
  if (!IsCurrent(generation, unused)) {
    return;
  }
  relay_pending_ = false;
  const auto id = *invitation.FindString("id");
  const auto* current = FindInvitation(id);
  if (!current || *current->FindString("status") != "accepted" ||
      (sending ? !source_files_.contains(id)
               : !accepted_receives_.contains(id))) {
    return;
  }
  const auto* relay = reply.body.FindDict("relay");
  const auto* room = relay ? relay->FindString("roomId") : nullptr;
  const auto* connection = relay ? relay->FindString("connectionId") : nullptr;
  const auto* token = relay ? relay->FindString("bearerToken") : nullptr;
  const auto expires = relay ? relay->FindDouble("expiresAt") : std::nullopt;
  if (reply.status != 200 || !room || room->empty() || room->size() > 64 ||
      !std::ranges::all_of(*room,
                           [](char c) {
                             return base::IsAsciiAlpha(c) ||
                                    base::IsAsciiDigit(c) || c == '_' ||
                                    c == '-';
                           }) ||
      !connection || !IsId(*connection) || !token || token->size() != 64 ||
      !std::ranges::all_of(*token,
                           [](char c) {
                             return (c >= '0' && c <= '9') ||
                                    (c >= 'a' && c <= 'f');
                           }) ||
      !expires || !std::isfinite(*expires) ||
      *expires <= base::Time::Now().InSecondsFSinceUnixEpoch()) {
    error_ = ApiError(reply);
    ForgetFile(id);
    return;
  }
  cross_transfer::TestJobOptions options;
  options.transport = "relay";
  options.role = sending ? "send" : "receive";
  options.host = "115.29.16.37";
  options.port = 443;
  options.ca_pem =
      XenonAccountService::GetForProfile(profile_)->GetTlsConfig().ca_pem;
  options.room_id = *room;
  options.connection_id = *connection;
  options.bearer_token = *token;
  options.task_id = id;
  options.expected_file_name = *invitation.FindString("fileName");
  options.expected_file_size =
      static_cast<uint64_t>(*invitation.FindDouble("fileSize"));
  if (sending) {
    const auto source = source_files_.find(id);
    if (source == source_files_.end()) {
      return;
    }
    options.source_path = source->second.path.AsUTF8Unsafe();
  } else {
    options.output_dir = DownloadPrefs::FromBrowserContext(profile_)
                             ->DownloadPath()
                             .AppendASCII("CrossTransfer")
                             .AppendASCII(id)
                             .AsUTF8Unsafe();
  }
  job_ = std::make_shared<cross_transfer::TestTransferJob>(std::move(options));
  job_running_ = true;
  base::ThreadPool::PostTaskAndReply(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](std::shared_ptr<cross_transfer::TestTransferJob> job) {
            job->Run();
          },
          job_),
      base::BindOnce(&XenonTransferService::OnJobFinished,
                     weak_factory_.GetWeakPtr(), generation_, id));
}

void XenonTransferService::OnJobFinished(uint64_t generation,
                                         std::string invitation_id) {
  StateCallback unused;
  if (!IsCurrent(generation, unused) || active_id_ != invitation_id) {
    return;
  }
  job_running_ = false;
  const auto snapshot = job_->snapshot();
  const bool success = snapshot.state == "completed";
  const bool receiver = active_role_ == "receive";
  if (success && !receiver) {
    ForgetFile(invitation_id);
    Refresh({});
    return;
  }
  if (!success && snapshot.state != "cancelled") {
    error_ = "文件传输失败，请重新发送";
  }
  QueueFinalization(std::move(invitation_id), success);
}

void XenonTransferService::QueueFinalization(std::string invitation_id,
                                             bool success) {
  const auto* invitation = FindInvitation(invitation_id);
  if (!invitation || IsTerminal(*invitation->FindString("status"))) {
    ForgetFile(invitation_id);
    return;
  }
  pending_finalization_ =
      PendingFinalization{std::move(invitation_id),
                          success,
                          base::Time::FromSecondsSinceUnixEpoch(
                              *invitation->FindDouble("expiresAt")),
                          {}};
  ++list_revision_;
  FinalizeFile();
}

void XenonTransferService::FinalizeFile() {
  OnSessionChanged();
  if (!pending_finalization_ || finalization_in_flight_) {
    return;
  }
  if (base::Time::Now() >= pending_finalization_->expires_at) {
    ForgetFile(pending_finalization_->id);
    ClearFinalization();
    Refresh({});
    return;
  }
  finalization_in_flight_ = true;
  XenonAccountService::GetForProfile(profile_)->RequestForSession(
      "POST",
      "/transfers/" + pending_finalization_->id +
          (pending_finalization_->success ? "/complete" : "/cancel"),
      base::DictValue().Set("deviceId", device_id_),
      base::BindOnce(&XenonTransferService::OnFinalized,
                     weak_factory_.GetWeakPtr(), generation_,
                     pending_finalization_->id));
}

void XenonTransferService::OnFinalized(uint64_t generation,
                                       std::string invitation_id,
                                       Response reply) {
  StateCallback unused;
  if (!IsCurrent(generation, unused) || !pending_finalization_ ||
      pending_finalization_->id != invitation_id) {
    return;
  }
  finalization_in_flight_ = false;
  const auto* input = reply.body.FindDict("transfer");
  auto invitation =
      input ? SanitizeInvitation(*input, device_id_) : std::nullopt;
  if (reply.status == 200 && invitation &&
      *invitation->FindString("id") == invitation_id &&
      IsTerminal(*invitation->FindString("status"))) {
    // Invalidate a list request that started before the terminal response.
    ++list_revision_;
    for (auto& item : invitations_) {
      if (*item.GetDict().FindString("id") == invitation_id) {
        item = base::Value(invitation->Clone());
        break;
      }
    }
    ForgetFile(invitation_id);
    ClearFinalization();
    error_.clear();
  } else if (reply.status == 0 || reply.status == 200 || reply.status == 408 ||
             reply.status == 429 || reply.status >= 500) {
    // Keep this single pending terminal update until acknowledged, expired,
    // or superseded by a terminal list entry. Polling must not restart the
    // already completed receive or hide the retry error.
    pending_finalization_->error = ApiError(reply);
    finalization_retry_timer_.Start(FROM_HERE, base::Seconds(2), this,
                                    &XenonTransferService::FinalizeFile);
  } else {
    error_ = ApiError(reply);
    ForgetFile(invitation_id);
    ClearFinalization();
  }
  Refresh({});
}

void XenonTransferService::ClearFinalization() {
  finalization_retry_timer_.Stop();
  pending_finalization_.reset();
  finalization_in_flight_ = false;
}

void XenonTransferService::StopLocalFile() {
  if (job_) {
    job_->Cancel();
  }
}

void XenonTransferService::ForgetFile(const std::string& invitation_id) {
  source_files_.erase(invitation_id);
  accepted_receives_.erase(invitation_id);
}

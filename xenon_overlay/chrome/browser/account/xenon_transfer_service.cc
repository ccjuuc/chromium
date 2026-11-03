// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/account/xenon_transfer_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "cross_transfer/auto_transport.h"
#include "cross_transfer/https_relay_channel.h"
#include "cross_transfer/test_job.h"
#include "cross_transfer/tls_transport_channel.h"
#include "xenon_overlay/chrome/browser/account/webrtc_connection_cache.h"
#include "xenon_overlay/chrome/browser/account/webrtc_transfer_channel.h"
#include "xenon_overlay/chrome/browser/xenon_prefs.h"

namespace {

constexpr char kUserDataKey[] = "xenon.own_device_transfer_service";
constexpr uint64_t kMaxFileSize = uint64_t{8} * 1024 * 1024 * 1024;
constexpr size_t kMaxTextBytes = 16 * 1024;
constexpr char kTextFileName[] = "message.txt";
constexpr std::array<int, 6> kSignalRetrySeconds = {2, 4, 8, 16, 30, 60};

// A worker retains the directory until all file handles have closed, even if
// the Profile or the invitation is removed in the meantime.
std::shared_ptr<base::ScopedTempDir> NewPayloadDirectory() {
  auto directory = std::shared_ptr<base::ScopedTempDir>(
      new base::ScopedTempDir, [](base::ScopedTempDir* value) {
        base::ThreadPool::PostTask(
            FROM_HERE,
            {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
            base::BindOnce([](std::unique_ptr<base::ScopedTempDir>) {},
                           std::unique_ptr<base::ScopedTempDir>(value)));
      });
  return directory->CreateUniqueTempDir() ? directory : nullptr;
}

xenon::WebRtcSignalState ReadWebRtc(
    const XenonAccountService::Response& response) {
  xenon::WebRtcSignalState result;
  const auto* connection = response.body.FindDict("webrtc");
  if (response.status != 200 || !connection) {
    return result;
  }
  const auto* route = connection->FindString("route");
  const auto* description = connection->Find("description");
  const auto* candidates = connection->FindList("candidates");
  const auto* servers = connection->FindList("iceServers");
  if (!route ||
      (*route != "pending" && *route != "direct" && *route != "relay") ||
      !description || !candidates || candidates->size() > 64 || !servers ||
      servers->size() > 8) {
    return result;
  }
  if (!description->is_none()) {
    const auto* item = description->GetIfDict();
    const auto* type = item ? item->FindString("type") : nullptr;
    const auto* sdp = item ? item->FindString("sdp") : nullptr;
    if (!type || (*type != "offer" && *type != "answer") || !sdp ||
        sdp->empty() || sdp->size() > 64 * 1024) {
      return {};
    }
    result.description = xenon::WebRtcDescription{*type, *sdp};
  }
  size_t candidate_bytes = 0;
  for (const auto& value : *candidates) {
    const auto* item = value.GetIfDict();
    const auto* mid = item ? item->FindString("sdpMid") : nullptr;
    const auto index = item ? item->FindInt("sdpMLineIndex") : std::nullopt;
    const auto* candidate = item ? item->FindString("candidate") : nullptr;
    if (!mid || mid->size() > 64 || !index || *index < 0 || *index > 65535 ||
        !candidate || candidate->empty() || candidate->size() > 2048) {
      return {};
    }
    candidate_bytes += candidate->size() + mid->size();
    if (candidate_bytes > 32 * 1024) {
      return {};
    }
    result.candidates.push_back({*mid, *index, *candidate});
  }
  for (const auto& value : *servers) {
    const auto* item = value.GetIfDict();
    const auto* urls = item ? item->FindList("urls") : nullptr;
    if (!urls || urls->size() > 8) {
      return {};
    }
    for (const auto& url : *urls) {
      const auto* text = url.GetIfString();
      if (!text || text->size() > 512 ||
          (!text->starts_with("stun:") && !text->starts_with("stuns:")) ||
          result.stun_urls.size() >= 8) {
        return {};
      }
      result.stun_urls.push_back(*text);
    }
  }
  result.ok = true;
  result.route = *route;
  return result;
}

// All callbacks below run on the transfer's owner worker. The server stores
// opaque snapshots; route convergence and SDP/ICE validation stay in the
// browser.
struct PeerChannelExchange {
  XenonAccountService::SessionRequest request;
  std::string id;
  std::string device;
  std::string peer;
  bool sending = false;
  int sequence = 0;
  int peer_sequence = -1;
  bool config_loaded = false;
  bool touched = false;
  uint64_t direct_min_bytes = 0;
  base::ListValue ice_servers;
  base::DictValue own;
  base::DictValue published;
  base::DictValue remote;

  void LoadConfig() {
    if (config_loaded) {
      return;
    }
    config_loaded = true;
    auto reply = request.Run("GET", "/transport-config", {});
    if (reply.status != 200) {
      return;
    }
    const auto limit = reply.body.FindDouble("directMinBytes");
    if (limit && std::isfinite(*limit) && *limit >= 0 &&
        *limit <= kMaxFileSize + 1 && std::floor(*limit) == *limit) {
      direct_min_bytes = static_cast<uint64_t>(*limit);
    }
    const auto* servers = reply.body.FindList("iceServers");
    if (servers && servers->size() <= 8) {
      ice_servers = servers->Clone();
    }
  }
  xenon::WebRtcSignalState Exchange() {
    if (published != own) {
      if (sequence == std::numeric_limits<int>::max()) {
        return {};
      }
      ++sequence;
      published = own.Clone();
    }
    base::DictValue body;
    body.Set("deviceId", device);
    body.Set("peerDeviceId", peer);
    body.Set("role", sending ? "sender" : "receiver");
    body.Set("sequence", sequence);
    body.Set("payload", published.Clone());
    touched = true;
    const auto reply =
        request.Run("POST", "/channels/" + id + "/exchange", std::move(body));
    const auto expires = reply.body.FindDouble("expiresAt");
    if (reply.status != 200 || !expires || !std::isfinite(*expires) ||
        *expires <= base::Time::Now().InSecondsFSinceUnixEpoch()) {
      return {};
    }
    if (const auto* incoming = reply.body.FindDict("peer")) {
      const auto version = incoming->FindInt("sequence");
      const auto* payload = incoming->FindDict("payload");
      if (!version || *version < 0 || !payload) {
        return {};
      }
      if (*version == peer_sequence && *payload != remote) {
        return {};
      }
      if (*version > peer_sequence) {
        const auto* route = payload->FindString("route");
        const auto* description = payload->Find("description");
        const auto* candidates = payload->FindList("candidates");
        if (!route ||
            (*route != "pending" && *route != "direct" && *route != "relay") ||
            !description || !candidates) {
          return {};
        }
        const auto* previous_route = remote.FindString("route");
        if (previous_route &&
            ((*previous_route == "relay" && *route != "relay") ||
             (*previous_route == "direct" && *route == "pending"))) {
          return {};
        }
        const auto* previous_description = remote.Find("description");
        if (previous_description && !previous_description->is_none() &&
            *description != *previous_description) {
          return {};
        }
        remote = payload->Clone();
        peer_sequence = *version;
      }
    } else {
      const auto* empty_peer = reply.body.Find("peer");
      if (!empty_peer || !empty_peer->is_none()) {
        return {};
      }
    }
    const auto* peer_route = remote.FindString("route");
    if (peer_route && *peer_route != "pending" && *peer_route != "direct" &&
        *peer_route != "relay") {
      return {};
    }
    if (peer_route && *peer_route == "relay") {
      own.Set("route", "relay");
    }
    const auto* own_route = own.FindString("route");
    const std::string route =
        *own_route == "relay" || (peer_route && *peer_route == "relay")
            ? "relay"
        : *own_route == "direct" && peer_route && *peer_route == "direct"
            ? "direct"
            : "pending";
    auto state = base::DictValue()
                     .Set("route", route)
                     .Set("iceServers", ice_servers.Clone());
    const auto* description = remote.Find("description");
    const auto* candidates = remote.FindList("candidates");
    state.Set("description",
              description ? description->Clone() : base::Value());
    state.Set("candidates",
              candidates ? candidates->Clone() : base::ListValue());
    return ReadWebRtc(
        {200, base::DictValue().Set("webrtc", std::move(state)), {}});
  }
  xenon::WebRtcSignalState SelectRoute(std::string route) {
    // A fallback decision is final even when publishing its snapshot fails.
    if (*own.FindString("route") != "relay") {
      own.Set("route", std::move(route));
    }
    return Exchange();
  }
};

cross_transfer::AutoConnectionOptions ConnectionOptions(
    const std::string& id,
    const std::string& device,
    const std::string& peer,
    uint64_t file_size,
    bool sending,
    bool use_stun,
    bool prefer_direct,
    std::shared_ptr<xenon::WebRtcConnectionCache> connections,
    std::string peer_key,
    XenonAccountService::SessionRequest request,
    base::OnceClosure* release_channel) {
  auto exchange = std::make_shared<PeerChannelExchange>();
  exchange->request = request;
  exchange->id = id;
  exchange->device = device;
  exchange->peer = peer;
  exchange->sending = sending;
  exchange->own.Set("route", "pending");
  exchange->own.Set("description", base::Value());
  exchange->own.Set("candidates", base::ListValue());
  *release_channel = base::BindOnce(
      [](std::shared_ptr<PeerChannelExchange> state) {
        if (!state->touched) {
          return;
        }
        // Releasing transport resources cannot hold the job's result or slot.
        // Each peer releases its own role; the server waits for both roles.
        base::ThreadPool::PostTask(
            FROM_HERE,
            {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
            base::BindOnce(
                [](std::shared_ptr<PeerChannelExchange> released) {
                  released->request.Run(
                      "POST", "/channels/" + released->id + "/release",
                      base::DictValue()
                          .Set("deviceId", released->device)
                          .Set("peerDeviceId", released->peer)
                          .Set("role",
                               released->sending ? "sender" : "receiver"));
                },
                std::move(state)));
      },
      exchange);
  cross_transfer::AutoConnectionOptions options;
  options.invitation_id = id;
  options.sending = sending;
  options.try_reuse = [connections, peer_key, id, sending](
                          const std::atomic<bool>* cancelled,
                          cross_transfer::TransportError* error) {
    return connections->TryReuse(peer_key, id, sending, cancelled, error);
  };
  options.retain_direct =
      [connections,
       peer_key](std::unique_ptr<cross_transfer::ITransportChannel> channel) {
        connections->Remember(peer_key, std::move(channel));
      };
  options.select_route = [exchange](std::string route) {
    const auto state = exchange->SelectRoute(std::move(route));
    cross_transfer::PeerConnectionState result;
    result.ok = state.ok;
    result.route = state.route;
    return result;
  };
  options.webrtc = [exchange, sending, use_stun, file_size, prefer_direct](
                       const std::atomic<bool>* cancelled,
                       cross_transfer::TransportError* error) {
    exchange->LoadConfig();
    if (!prefer_direct && file_size < exchange->direct_min_bytes) {
      if (error) {
        *error = cross_transfer::TransportError::kIo;
      }
      return std::unique_ptr<cross_transfer::ITransportChannel>();
    }
    xenon::WebRtcConnectionOptions rtc;
    rtc.sending = sending;
    rtc.use_stun = use_stun;
    rtc.invitation_id = exchange->id;
    rtc.exchange = [exchange](
                       std::optional<xenon::WebRtcDescription> description,
                       std::vector<xenon::WebRtcIceCandidate> candidates) {
      if (description) {
        auto value = base::DictValue()
                         .Set("type", description->type)
                         .Set("sdp", description->sdp);
        const auto* old = exchange->own.Find("description");
        if (old && !old->is_none() && *old != base::Value(value.Clone())) {
          return xenon::WebRtcSignalState();
        }
        exchange->own.Set("description", std::move(value));
      }
      auto* values = exchange->own.FindList("candidates");
      for (const auto& candidate : candidates) {
        bool duplicate = false;
        for (const auto& existing : *values) {
          if (*existing.GetDict().FindString("candidate") ==
                  candidate.candidate &&
              *existing.GetDict().FindString("sdpMid") == candidate.sdp_mid &&
              existing.GetDict().FindInt("sdpMLineIndex") ==
                  candidate.sdp_mline_index) {
            duplicate = true;
            break;
          }
        }
        if (!duplicate) {
          if (values->size() >= 64) {
            return xenon::WebRtcSignalState();
          }
          values->Append(base::DictValue()
                             .Set("sdpMid", candidate.sdp_mid)
                             .Set("sdpMLineIndex", candidate.sdp_mline_index)
                             .Set("candidate", candidate.candidate));
        }
      }
      return exchange->Exchange();
    };
    rtc.confirm_direct = [exchange]() {
      const auto state = exchange->SelectRoute("direct");
      return state.ok ? state.route : std::string();
    };
    return xenon::ConnectWebRtc(rtc, cancelled, error);
  };
  options.relay = [request, id, device, peer, sending, exchange](
                      const std::atomic<bool>* cancelled,
                      cross_transfer::TransportError* error)
      -> std::unique_ptr<cross_transfer::ITransportChannel> {
    if (cancelled->load()) {
      if (error) {
        *error = cross_transfer::TransportError::kCancelled;
      }
      return nullptr;
    }
    exchange->touched = true;
    const auto response =
        request.Run("POST", "/channels/" + id + "/relay",
                    base::DictValue()
                        .Set("deviceId", device)
                        .Set("peerDeviceId", peer)
                        .Set("role", sending ? "sender" : "receiver"));
    const auto* relay = response.body.FindDict("relay");
    const auto* room = relay ? relay->FindString("roomId") : nullptr;
    const auto* connection =
        relay ? relay->FindString("connectionId") : nullptr;
    const auto* token = relay ? relay->FindString("bearerToken") : nullptr;
    if (response.status != 200 || !room || *room != "own_" + id ||
        !connection || *connection != id || !token || token->size() != 64) {
      if (error) {
        *error = cross_transfer::TransportError::kSecurity;
      }
      return nullptr;
    }
    cross_transfer::HttpsRelayOptions relay_options;
    relay_options.host = "115.29.16.37";
    relay_options.port = 443;
    relay_options.ca_pem = XenonAccountService::GetTlsConfig().ca_pem;
    relay_options.room_id = *room;
    relay_options.connection_id = *connection;
    relay_options.bearer_token = *token;
    relay_options.role = sending ? cross_transfer::RelayRole::kSender
                                 : cross_transfer::RelayRole::kReceiver;
    return cross_transfer::HttpsRelayChannel::Connect(std::move(relay_options),
                                                      cancelled, error);
  };
  return options;
}

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
  const auto* mode = input.FindString("payloadMode");
  const auto* transport_policy = input.FindString("transportPolicy");
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
      !text || text->size() > kMaxTextBytes ||
      text->find('\0') != std::string::npos || !base::IsStringUTF8(*text) ||
      !name || name->size() > 1024 || !base::IsStringUTF8(*name) || !size ||
      !std::isfinite(*size) || *size < 0 || std::floor(*size) != *size ||
      *size > kMaxFileSize || !created || !std::isfinite(*created) ||
      !expires || !std::isfinite(*expires)) {
    return std::nullopt;
  }
  if (!mode || *mode != "stream") {
    return std::nullopt;
  }
  if (transport_policy && *transport_policy != "auto" &&
      *transport_policy != "relay") {
    return std::nullopt;
  }
  if (*kind == "text") {
    if ((!text->empty() && static_cast<double>(text->size()) != *size) ||
        *name != kTextFileName || *size < 1 || *size > kMaxTextBytes) {
      return std::nullopt;
    }
  } else if (*kind == "file") {
    const auto path = base::FilePath::FromUTF8Unsafe(*name);
    if (name->empty() || path.BaseName() != path || path.ReferencesParent() ||
        name->find_first_of("/\\:\0", 0, 4) != std::string::npos ||
        !text->empty()) {
      return std::nullopt;
    }
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
      .Set("payloadMode", "stream")
      .Set("transportPolicy", transport_policy ? *transport_policy : "auto")
      .Set("preferDirect", input.FindBool("preferDirect").value_or(false))
      .Set("text", *text)
      .Set("fileName", *name)
      .Set("fileSize", *size)
      .Set("status", *status)
      .Set("createdAt", *created)
      .Set("expiresAt", *expires);
}

std::string NewId() {
  return base::HexEncodeLower(base::RandBytesAsString(16));
}
bool IsOutgoingSignal(const base::DictValue& message,
                      const std::string& id,
                      const std::string& device) {
  const auto* identity = message.FindString("id");
  const auto* from = message.FindString("fromDeviceId");
  const auto* to = message.FindString("toDeviceId");
  const auto* payload = message.FindDict("payload");
  const auto* task = payload ? payload->FindString("taskId") : nullptr;
  const auto* type = payload ? payload->FindString("type") : nullptr;
  return message.size() == 4 && identity && *identity == id && IsId(id) &&
         from && *from == device && to && IsId(*to) && *to != device && task &&
         IsId(*task) && type &&
         (*type == "offer" || *type == "accept" || *type == "reject" ||
          *type == "cancel");
}
void PruneTimedEntries(base::DictValue* entries, double now, size_t limit) {
  std::vector<std::string> expired;
  for (const auto [id, value] : *entries) {
    const auto* item = value.GetIfDict();
    const auto deadline =
        item ? item->FindDouble("expiresAt") : value.GetIfDouble();
    if (!IsId(id) || !deadline || !std::isfinite(*deadline) ||
        *deadline <= now || *deadline > now + base::Minutes(10).InSecondsF()) {
      expired.push_back(id);
    }
  }
  for (const auto& id : expired) {
    entries->Remove(id);
  }
  while (entries->size() > limit) {
    entries->Remove(entries->begin()->first);
  }
}

}  // namespace

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
    : profile_(profile),
      direct_connections_(std::make_shared<xenon::WebRtcConnectionCache>()) {}
XenonTransferService::~XenonTransferService() {
  StopLocalFile();
  direct_connections_->Clear();
}
void XenonTransferService::ResetDirectConnections() {
  direct_connections_->Clear();
  direct_connections_ = std::make_shared<xenon::WebRtcConnectionCache>();
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
  StopLocalFile();
  ResetDirectConnections();
  signal_retry_timer_.Stop();
  signal_in_flight_id_.clear();
  outbox_write_pending_ = false;
  ++outbox_write_revision_;
  account_id_ = account_id;
  device_id_ = device_id;
  polling_ = operation_busy_ = relay_pending_ = job_running_ = false;
  job_.reset();
  active_id_.clear();
  active_role_.clear();
  active_name_.clear();
  active_invitation_.clear();
  file_error_.clear();
  active_text_ = false;
  source_files_.clear();
  accepted_receives_.clear();
  file_attempts_.clear();
  local_tasks_.clear();
  direct_peers_.clear();
  signal_outbox_.clear();
  seen_signals_.clear();
  cancelled_before_offer_.clear();
  error_.clear();
  LoadLocalTasks();
  PumpSignalOutbox();
}

void XenonTransferService::LoadLocalTasks() {
  if (account_id_.empty() || device_id_.empty()) {
    return;
  }
  const auto& history =
      profile_->GetPrefs()->GetDict(xenon::prefs::kTransferHistory);
  const auto* scope = history.FindDict(account_id_ + ":" + device_id_);
  if (scope && scope->FindString("accountId") &&
      scope->FindString("deviceId") &&
      *scope->FindString("accountId") == account_id_ &&
      *scope->FindString("deviceId") == device_id_) {
    if (const auto* peers = scope->FindDict("directPeers")) {
      const double now = base::Time::Now().InSecondsFSinceUnixEpoch();
      for (const auto [peer, value] : *peers) {
        const auto success = value.GetIfDouble();
        if (IsId(peer) && peer != device_id_ && success &&
            std::isfinite(*success) && *success > 0 &&
            *success <= now + base::Minutes(5).InSecondsF() &&
            direct_peers_.size() < 32) {
          direct_peers_.Set(peer, *success);
        }
      }
    }
    if (const auto* tasks = scope->FindDict("tasks")) {
      for (const auto [id, value] : *tasks) {
        auto task = value.is_dict()
                        ? SanitizeInvitation(value.GetDict(), device_id_)
                        : std::nullopt;
        if (task && *task->FindString("id") == id &&
            local_tasks_.size() < 128) {
          const double updated = value.GetDict()
                                     .FindDouble("updatedAt")
                                     .value_or(*task->FindDouble("createdAt"));
          if (std::isfinite(updated)) {
            task->Set("updatedAt", updated);
          }
          const auto* error = value.GetDict().FindString("error");
          if (error && error->size() <= 1024) {
            task->Set("error", *error);
          }
          const auto* saved = value.GetDict().FindString("savedPath");
          if (saved && saved->size() <= 4096 && base::IsStringUTF8(*saved)) {
            task->Set("savedPath", *saved);
          }
          local_tasks_.Set(id, std::move(*task));
        }
      }
    }
    if (const auto* outbox = scope->FindDict("outbox")) {
      for (const auto [id, value] : *outbox) {
        const auto* entry = value.GetIfDict();
        const auto* message = entry ? entry->FindDict("message") : nullptr;
        const auto deadline =
            entry ? entry->FindDouble("deadline") : std::nullopt;
        const auto next =
            entry ? entry->FindDouble("nextAttemptAt") : std::nullopt;
        const auto tries = entry ? entry->FindInt("attempts") : std::nullopt;
        if (message && IsOutgoingSignal(*message, id, device_id_) && deadline &&
            next && tries && std::isfinite(*deadline) && std::isfinite(*next) &&
            *tries >= 0 && *tries <= 5 &&
            *deadline > base::Time::Now().InSecondsFSinceUnixEpoch() &&
            signal_outbox_.size() < 128) {
          signal_outbox_.Set(id, entry->Clone());
        }
      }
    }
    if (const auto* seen = scope->FindDict("seenSignals")) {
      seen_signals_ = seen->Clone();
    }
    if (const auto* cancelled = scope->FindDict("tombstones")) {
      cancelled_before_offer_ = cancelled->Clone();
    }
  }
  std::vector<std::string> interrupted;
  for (const auto [id, value] : local_tasks_) {
    const auto& task = value.GetDict();
    if (!IsTerminal(*task.FindString("status")) &&
        (*task.FindString("status") == "accepted" ||
         *task.FindString("senderDeviceId") == device_id_)) {
      interrupted.push_back(id);
    }
  }
  for (const auto& id : interrupted) {
    FailTask(id, "传输已中断，请重新发送");
  }
  ExpireLocalTasks();
  PersistLocalTasks();
}

void XenonTransferService::PersistLocalTasks() {
  if (account_id_.empty() || device_id_.empty()) {
    return;
  }
  const double now = base::Time::Now().InSecondsFSinceUnixEpoch();
  std::vector<std::string> remove;
  for (const auto [id, value] : local_tasks_) {
    const auto& task = value.GetDict();
    if (IsTerminal(*task.FindString("status")) &&
        now - task.FindDouble("updatedAt")
                    .value_or(*task.FindDouble("createdAt")) >
            base::Days(7).InSecondsF()) {
      remove.push_back(id);
    }
  }
  for (const auto& id : remove) {
    local_tasks_.Remove(id);
  }
  while (local_tasks_.size() > 64) {
    std::string oldest;
    double time = std::numeric_limits<double>::infinity();
    for (const auto [id, value] : local_tasks_) {
      const auto& task = value.GetDict();
      const double updated =
          task.FindDouble("updatedAt").value_or(*task.FindDouble("createdAt"));
      if (id != active_id_ && IsTerminal(*task.FindString("status")) &&
          updated < time) {
        oldest = id;
        time = updated;
      }
    }
    if (oldest.empty()) {
      break;
    }
    local_tasks_.Remove(oldest);
  }
  std::erase_if(file_attempts_,
                [this](const auto& id) { return !local_tasks_.contains(id); });
  PruneTimedEntries(&seen_signals_, now, 256);
  PruneTimedEntries(&cancelled_before_offer_, now, 128);
  auto history =
      profile_->GetPrefs()->GetDict(xenon::prefs::kTransferHistory).Clone();
  const auto key = account_id_ + ":" + device_id_;
  history.Set(key, base::DictValue()
                       .Set("accountId", account_id_)
                       .Set("deviceId", device_id_)
                       .Set("tasks", local_tasks_.Clone())
                       .Set("directPeers", direct_peers_.Clone())
                       .Set("outbox", signal_outbox_.Clone())
                       .Set("seenSignals", seen_signals_.Clone())
                       .Set("tombstones", cancelled_before_offer_.Clone())
                       .Set("updatedAt", now));
  while (history.size() > 4) {
    std::string oldest;
    double time = std::numeric_limits<double>::infinity();
    for (const auto [scope_key, value] : history) {
      const auto* entry = value.GetIfDict();
      const double updated =
          entry ? entry->FindDouble("updatedAt").value_or(0) : 0;
      if (scope_key != key && updated < time) {
        oldest = scope_key;
        time = updated;
      }
    }
    if (oldest.empty()) {
      break;
    }
    history.Remove(oldest);
  }
  profile_->GetPrefs()->SetDict(xenon::prefs::kTransferHistory,
                                std::move(history));
}

void XenonTransferService::ExpireLocalTasks() {
  std::vector<std::string> expired;
  const double now = base::Time::Now().InSecondsFSinceUnixEpoch();
  for (const auto [id, value] : local_tasks_) {
    const auto& task = value.GetDict();
    // Once a worker owns the task, its I/O and stage deadlines apply. A slow
    // but progressing direct payload must not inherit an invitation deadline.
    if (id == active_id_ && (job_running_ || relay_pending_)) {
      continue;
    }
    if (!IsTerminal(*task.FindString("status")) &&
        *task.FindDouble("expiresAt") <= now) {
      expired.push_back(id);
    }
  }
  for (const auto& id : expired) {
    SetTaskState(id, "expired", "传输已过期");
    QueueSignal(id, "cancel");
  }
}

bool XenonTransferService::WasDirectSuccessful(const std::string& peer) const {
  return direct_peers_.contains(peer);
}

void XenonTransferService::RememberDirectSuccess(const std::string& peer) {
  if (!IsId(peer) || peer == device_id_) {
    return;
  }
  direct_peers_.Set(peer, base::Time::Now().InSecondsFSinceUnixEpoch());
  while (direct_peers_.size() > 32) {
    std::string oldest;
    double oldest_time = std::numeric_limits<double>::infinity();
    for (const auto [id, value] : direct_peers_) {
      if (id != peer && value.GetDouble() < oldest_time) {
        oldest = id;
        oldest_time = value.GetDouble();
      }
    }
    if (oldest.empty()) {
      break;
    }
    direct_peers_.Remove(oldest);
  }
  PersistLocalTasks();
}

void XenonTransferService::SetTaskState(const std::string& id,
                                        std::string status,
                                        std::string error) {
  auto* task = local_tasks_.FindDict(id);
  if (!task || *task->FindString("status") == "completed") {
    return;
  }
  task->Set("status", status);
  task->Set("error", error);
  task->Set("updatedAt", base::Time::Now().InSecondsFSinceUnixEpoch());
  if (IsTerminal(status)) {
    ForgetFile(id);
    std::vector<std::string> obsolete;
    for (const auto [message_id, value] : signal_outbox_) {
      const auto* payload =
          value.GetDict().FindDict("message")->FindDict("payload");
      if (*payload->FindString("taskId") == id &&
          (*payload->FindString("type") == "offer" ||
           *payload->FindString("type") == "accept")) {
        obsolete.push_back(message_id);
      }
    }
    for (const auto& message_id : obsolete) {
      signal_outbox_.Remove(message_id);
    }
    if (active_id_ == id) {
      file_error_ = error;
      if (status != "completed") {
        StopLocalFile();
      }
    }
  }
  PersistLocalTasks();
}
void XenonTransferService::FailTask(const std::string& id,
                                    std::string error,
                                    bool notify_peer) {
  const auto* task = FindInvitation(id);
  if (!task || IsTerminal(*task->FindString("status"))) {
    return;
  }
  SetTaskState(id, "failed", std::move(error));
  if (notify_peer) {
    QueueSignal(id, "cancel");
  }
}

base::DictValue XenonTransferService::GetState() const {
  auto job = base::DictValue()
                 .Set("id", active_id_)
                 .Set("transferId", active_id_)
                 .Set("role", active_role_)
                 .Set("kind", active_text_ ? "text" : "file")
                 .Set("fileName", active_name_)
                 .Set("active", HasFileWork())
                 .Set("state", "idle")
                 .Set("status", "")
                 .Set("error", "")
                 .Set("savedPath", "")
                 .Set("transport", "auto")
                 .Set("protocol", "")
                 .Set("transferredBytes", 0.0)
                 .Set("totalBytes", 0.0)
                 .Set("percent", 0.0);
  if (job_) {
    const auto snapshot = job_->snapshot();
    job.Set("state", snapshot.state);
    job.Set("status", snapshot.status);
    job.Set("error", file_error_.empty() ? snapshot.error : file_error_);
    job.Set("savedPath", active_text_ ? "" : snapshot.saved_path);
    job.Set("transport", snapshot.transport);
    job.Set("protocol", snapshot.protocol);
    job.Set("transferredBytes", static_cast<double>(snapshot.bytes));
    job.Set("totalBytes", static_cast<double>(snapshot.total));
    job.Set("percent", snapshot.percent);
  } else if (relay_pending_) {
    job.Set("state", "connecting");
    job.Set("status", "正在建立传输连接…");
  }
  const auto* current = FindInvitation(active_id_);
  if (current) {
    const auto& state = *current->FindString("status");
    job.Set("totalBytes", *current->FindDouble("fileSize"));
    if (state == "completed") {
      job.Set("state", "completed");
      job.Set("status", active_role_ == "send"
                            ? "对方已确认完整接收"
                            : (active_text_ ? "文本已收到" : "文件已保存"));
      job.Set("error", "");
      job.Set("percent", 100.0);
      job.Set("transferredBytes", *current->FindDouble("fileSize"));
    } else if (IsTerminal(state)) {
      job.Set("state", state);
      job.Set("error", current->FindString("error")
                           ? *current->FindString("error")
                           : "");
    } else if (!job_ && !relay_pending_) {
      job.Set("state", state == "pending" ? "waiting" : "connecting");
      job.Set("status",
              state == "pending" ? "等待对方接收" : "正在建立传输连接…");
    }
  }
  base::ListValue tasks;
  for (const auto [id, value] : local_tasks_) {
    tasks.Append(value.Clone());
  }
  return base::DictValue()
      .Set("useStun",
           profile_->GetPrefs()->GetBoolean(xenon::prefs::kTransferUseStun))
      .Set("transfers", std::move(tasks))
      .Set("job", std::move(job))
      .Set("busy", operation_busy_)
      .Set("error", error_);
}
void XenonTransferService::SetUseStun(bool enabled, StateCallback callback) {
  if (profile_->GetPrefs()->GetBoolean(xenon::prefs::kTransferUseStun) !=
      enabled) {
    ResetDirectConnections();
  }
  profile_->GetPrefs()->SetBoolean(xenon::prefs::kTransferUseStun, enabled);
  if (callback) {
    std::move(callback).Run(GetState());
  }
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

void XenonTransferService::CreateInvitation(std::string receiver_id,
                                            SourceFile source,
                                            StateCallback callback) {
  if (local_tasks_.size() >= 128 || signal_outbox_.size() >= 128) {
    Finish(std::move(callback), "待处理传输过多，请稍后再发送");
    return;
  }
  const std::string id = NewId();
  const auto now = base::Time::Now();
  std::string sender_name = "本机";
  std::string receiver_name = "另一台设备";
  auto account = XenonAccountService::GetForProfile(profile_)->GetState();
  if (const auto* devices = account.FindList("devices")) {
    for (const auto& device : *devices) {
      const auto* data = device.GetIfDict();
      const auto* name = data ? data->FindString("displayName") : nullptr;
      const auto* device_id = data ? data->FindString("id") : nullptr;
      if (name && device_id && name->size() <= 512) {
        if (*device_id == receiver_id) {
          receiver_name = *name;
        }
        if (*device_id == device_id_) {
          sender_name = *name;
        }
      }
    }
  }
  auto task =
      base::DictValue()
          .Set("id", id)
          .Set("senderDeviceId", device_id_)
          .Set("receiverDeviceId", receiver_id)
          .Set("senderName", sender_name)
          .Set("receiverName", receiver_name)
          .Set("direction", "outgoing")
          .Set("kind", source.text.empty() ? "file" : "text")
          .Set("payloadMode", "stream")
          .Set("transportPolicy", "auto")
          .Set("preferDirect", WasDirectSuccessful(receiver_id))
          .Set("text", source.text)
          .Set("fileName", source.name)
          .Set("fileSize", static_cast<double>(source.size))
          .Set("status", "pending")
          .Set("createdAt", now.InSecondsFSinceUnixEpoch())
          .Set("expiresAt", (now + base::Minutes(5)).InSecondsFSinceUnixEpoch())
          .Set("updatedAt", now.InSecondsFSinceUnixEpoch());
  source_files_.emplace(id, std::move(source));
  SetActiveFile(task);
  local_tasks_.Set(id, std::move(task));
  QueueSignal(id, "offer");
  Finish(std::move(callback));
}
void XenonTransferService::Respond(std::string id,
                                   std::string action,
                                   StateCallback callback) {
  if (!Ready(callback)) {
    return;
  }
  ExpireLocalTasks();
  if (operation_busy_) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  const auto* task = FindInvitation(id);
  if (!task ||
      (action != "accept" && action != "reject" && action != "cancel") ||
      (action != "cancel" &&
       *task->FindString("receiverDeviceId") != device_id_)) {
    Finish(std::move(callback), "传输状态已变化");
    return;
  }
  if (IsTerminal(*task->FindString("status"))) {
    Finish(std::move(callback));
    return;
  }
  if (action == "accept") {
    if (HasFileWork() || *task->FindString("status") != "pending") {
      Finish(std::move(callback), "请等待当前传输结束后再接收");
      return;
    }
    if (signal_outbox_.size() >= 128) {
      Finish(std::move(callback), "待发送通知过多，请稍后再接收");
      return;
    }
    SetActiveFile(*task);
    accepted_receives_.insert(id);
    local_tasks_.FindDict(id)->Set(
        "preferDirect",
        task->FindBool("preferDirect").value_or(false) ||
            WasDirectSuccessful(*task->FindString("senderDeviceId")));
    local_tasks_.FindDict(id)->Set(
        "expiresAt",
        (base::Time::Now() + base::Minutes(30)).InSecondsFSinceUnixEpoch());
    SetTaskState(id, "accepted");
    QueueSignal(id, "accept");
    ConsiderStartingFile();
  } else {
    SetTaskState(id, action == "reject" ? "rejected" : "cancelled");
    QueueSignal(id, action);
  }
  Finish(std::move(callback));
}
void XenonTransferService::Cancel(StateCallback callback) {
  if (active_id_.empty()) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  Respond(active_id_, "cancel", std::move(callback));
}
const base::DictValue* XenonTransferService::FindInvitation(
    const std::string& id) const {
  return local_tasks_.FindDict(id);
}
bool XenonTransferService::HasFileWork() const {
  return job_running_ || relay_pending_ || !source_files_.empty() ||
         !accepted_receives_.empty();
}
void XenonTransferService::SetActiveFile(const base::DictValue& task) {
  active_invitation_ = task.Clone();
  active_id_ = *task.FindString("id");
  active_role_ =
      *task.FindString("senderDeviceId") == device_id_ ? "send" : "receive";
  active_name_ = *task.FindString("fileName");
  active_text_ = *task.FindString("kind") == "text";
  job_.reset();
  file_error_.clear();
}
void XenonTransferService::ConsiderStartingFile() {
  if (job_running_ || relay_pending_) {
    return;
  }
  for (const auto [id, value] : local_tasks_) {
    const auto& task = value.GetDict();
    if (*task.FindString("status") != "accepted" ||
        file_attempts_.contains(id)) {
      continue;
    }
    const bool sending = *task.FindString("senderDeviceId") == device_id_;
    if (sending ? source_files_.contains(id)
                : accepted_receives_.contains(id)) {
      StartJob(task, sending);
      return;
    }
  }
}
void XenonTransferService::SaveText(const std::string& id,
                                    const std::string& text) {
  if (auto* task = local_tasks_.FindDict(id)) {
    task->Set("text", text);
  }
}
void XenonTransferService::RecordCompletedTransfer(const std::string& id) {
  auto* task = local_tasks_.FindDict(id);
  if (!task && id == active_id_ && !active_invitation_.empty()) {
    local_tasks_.Set(id, active_invitation_.Clone());
    task = local_tasks_.FindDict(id);
  }
  if (!task) {
    return;
  }
  task->Set("status", "completed");
  task->Set("error", "");
  task->Set("updatedAt", base::Time::Now().InSecondsFSinceUnixEpoch());
  if (id == active_id_ && active_role_ == "receive" && !active_text_ && job_) {
    const auto saved = job_->snapshot().saved_path;
    if (!saved.empty()) {
      task->Set("savedPath", saved);
    }
  }
  std::vector<std::string> obsolete;
  for (const auto [message_id, value] : signal_outbox_) {
    if (*value.GetDict().FindDict("message")->FindDict("payload")->FindString(
            "taskId") == id) {
      obsolete.push_back(message_id);
    }
  }
  for (const auto& message_id : obsolete) {
    signal_outbox_.Remove(message_id);
  }
  ForgetFile(id);
  file_attempts_.insert(id);
  if (active_id_ == id) {
    file_error_.clear();
  }
  PersistLocalTasks();
  profile_->GetPrefs()->CommitPendingWrite();
}
void XenonTransferService::OnJobFinished(uint64_t generation,
                                         std::string id,
                                         JobResult result) {
  StateCallback unused;
  if (!IsCurrent(generation, unused) || active_id_ != id) {
    return;
  }
  job_running_ = false;
  const auto* task = FindInvitation(id);
  if (task && *task->FindString("status") == "completed") {
    ForgetFile(id);
    return;
  }
  if (result.success) {
    if (job_ && job_->snapshot().transport == "direct" && task) {
      RememberDirectSuccess(*task->FindString(
          active_role_ == "send" ? "receiverDeviceId" : "senderDeviceId"));
    }
    if (active_text_ && active_role_ == "receive") {
      SaveText(id, result.text);
    }
    RecordCompletedTransfer(id);
    return;
  }
  if (!task || IsTerminal(*task->FindString("status"))) {
    ForgetFile(id);
    return;
  }
  HandleJobFailure(id, job_->snapshot().completion_requested);
}
void XenonTransferService::HandleJobFailure(const std::string& id,
                                            bool completion_requested) {
  if (id == active_id_) {
    job_running_ = false;
  }
  const bool uncertain = active_role_ == "send" && completion_requested;
  FailTask(id,
           uncertain ? "发送结果未确认；对方可能已收到，请先核实再重新发送"
                     : "传输失败，请重新发送",
           !uncertain);
}
void XenonTransferService::StopLocalFile() {
  if (job_) {
    job_->Cancel();
  }
}
void XenonTransferService::ForgetFile(const std::string& id) {
  source_files_.erase(id);
  accepted_receives_.erase(id);
}

void XenonTransferService::Refresh(StateCallback callback, bool retry_signals) {
  OnSessionChanged();
  ExpireLocalTasks();
  PumpSignalOutbox(retry_signals);
  if (polling_ || account_id_.empty() || device_id_.empty()) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  polling_ = true;
  XenonAccountService::GetForProfile(profile_)->RequestForSession(
      "GET", "/devices/" + device_id_ + "/signals", {},
      base::BindOnce(&XenonTransferService::OnSignals,
                     weak_factory_.GetWeakPtr(), generation_,
                     std::move(callback)));
}

void XenonTransferService::OnSignals(uint64_t generation,
                                     StateCallback callback,
                                     Response reply) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  const auto* signals = reply.body.FindList("signals");
  if (reply.status != 200 || !signals || signals->size() > 64) {
    polling_ = false;
    error_ = ApiError(reply);
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  error_.clear();
  base::ListValue acknowledgements;
  for (const auto& value : *signals) {
    const auto* signal = value.GetIfDict();
    const auto* id = signal ? signal->FindString("id") : nullptr;
    const auto* target = signal ? signal->FindString("toDeviceId") : nullptr;
    if (!id || !IsId(*id) || !target || *target != device_id_) {
      continue;
    }
    if (!seen_signals_.contains(*id)) {
      ApplySignal(*signal);
      seen_signals_.Set(
          *id,
          (base::Time::Now() + base::Minutes(10)).InSecondsFSinceUnixEpoch());
    }
    acknowledgements.Append(*id);
  }
  PersistLocalTasks();
  // Consume only after the local transition and dedup key have reached disk.
  // A failed or lost ACK is harmless: the next poll acknowledges it again.
  profile_->GetPrefs()->CommitPendingWrite(base::BindOnce(
      &XenonTransferService::AcknowledgeSignals, weak_factory_.GetWeakPtr(),
      generation_, std::move(acknowledgements), std::move(callback)));
}

void XenonTransferService::AcknowledgeSignals(uint64_t generation,
                                              base::ListValue ids,
                                              StateCallback callback) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  if (ids.empty()) {
    polling_ = false;
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  XenonAccountService::GetForProfile(profile_)->RequestForSession(
      "POST", "/devices/" + device_id_ + "/signals/ack",
      base::DictValue().Set("messageIds", std::move(ids)),
      base::BindOnce(
          [](base::WeakPtr<XenonTransferService> service, uint64_t generation,
             StateCallback callback, Response) {
            if (!service || !service->IsCurrent(generation, callback)) {
              return;
            }
            service->polling_ = false;
            if (callback) {
              std::move(callback).Run(service->GetState());
            }
          },
          weak_factory_.GetWeakPtr(), generation_, std::move(callback)));
}

void XenonTransferService::ApplySignal(const base::DictValue& signal) {
  const auto* from = signal.FindString("fromDeviceId");
  const auto* to = signal.FindString("toDeviceId");
  const auto* payload = signal.FindDict("payload");
  const auto* id = payload ? payload->FindString("taskId") : nullptr;
  const auto* type = payload ? payload->FindString("type") : nullptr;
  const auto expires = signal.FindDouble("expiresAt");
  if (!from || !IsId(*from) || !to || *to != device_id_ ||
      *from == device_id_ || !id || !IsId(*id) || !type || !expires ||
      !std::isfinite(*expires) ||
      *expires <= base::Time::Now().InSecondsFSinceUnixEpoch()) {
    return;
  }
  const auto* current = FindInvitation(*id);
  if (*type == "offer") {
    const auto* offer = payload->FindDict("offer");
    const auto* from_name = signal.FindString("fromName");
    const auto* to_name = signal.FindString("toName");
    const auto created = offer ? offer->FindDouble("createdAt") : std::nullopt;
    const auto deadline = offer ? offer->FindDouble("expiresAt") : std::nullopt;
    const auto prefer_direct =
        offer ? offer->FindBool("preferDirect") : std::nullopt;
    const double now = base::Time::Now().InSecondsFSinceUnixEpoch();
    if (!offer || !from_name || !to_name || !created || !deadline ||
        !prefer_direct || !std::isfinite(*created) ||
        !std::isfinite(*deadline) || *created <= 0 || *deadline <= *created ||
        *deadline - *created > base::Minutes(5).InSecondsF() ||
        *created > now + base::Minutes(5).InSecondsF()) {
      return;
    }
    auto input = offer->Clone();
    input.Set("id", *id);
    input.Set("senderDeviceId", *from);
    input.Set("receiverDeviceId", *to);
    input.Set("senderName", *from_name);
    input.Set("receiverName", *to_name);
    input.Set("status", "pending");
    input.Set("payloadMode", "stream");
    input.Set("transportPolicy", "auto");
    input.Set("preferDirect", *prefer_direct || WasDirectSuccessful(*from));
    input.Set("text", "");
    input.Set("createdAt", *created);
    // The sender's original deadline survives retries and delayed delivery.
    // The mailbox envelope's TTL only controls retention of this message.
    input.Set("expiresAt", *deadline);
    auto task = SanitizeInvitation(input, device_id_);
    if (!task || current || local_tasks_.size() >= 128) {
      return;
    }
    task->Set("updatedAt", base::Time::Now().InSecondsFSinceUnixEpoch());
    const auto* tombstone = cancelled_before_offer_.FindDict(*id);
    if (tombstone && tombstone->FindString("peerDeviceId") &&
        *tombstone->FindString("peerDeviceId") == *from &&
        tombstone->FindDouble("expiresAt").value_or(0) >
            base::Time::Now().InSecondsFSinceUnixEpoch()) {
      task->Set("status", "cancelled");
    }
    local_tasks_.Set(*id, std::move(*task));
    if (*deadline <= now) {
      SetTaskState(*id, "expired", "传输已过期");
      QueueSignal(*id, "cancel");
    }
    PersistLocalTasks();
    return;
  }
  if (*type != "accept" && *type != "reject" && *type != "cancel") {
    return;
  }
  if (!current) {
    if (*type == "cancel") {
      cancelled_before_offer_.Set(
          *id, base::DictValue()
                   .Set("peerDeviceId", *from)
                   .Set("expiresAt", (base::Time::Now() + base::Minutes(10))
                                         .InSecondsFSinceUnixEpoch()));
    }
    return;
  }
  const auto& sender = *current->FindString("senderDeviceId");
  const auto& receiver = *current->FindString("receiverDeviceId");
  if ((sender == device_id_ ? receiver : sender) != *from ||
      IsTerminal(*current->FindString("status"))) {
    return;
  }
  if (*type == "accept") {
    const auto prefer_direct = payload->FindBool("preferDirect");
    if (!prefer_direct) {
      return;
    }
    if (sender != device_id_ || *current->FindString("status") != "pending") {
      return;
    }
    if (*current->FindDouble("expiresAt") <=
        base::Time::Now().InSecondsFSinceUnixEpoch()) {
      SetTaskState(*id, "expired", "传输已过期");
      QueueSignal(*id, "cancel");
      return;
    }
    local_tasks_.FindDict(*id)->Set(
        "preferDirect", *prefer_direct ||
                            current->FindBool("preferDirect").value_or(false) ||
                            WasDirectSuccessful(*from));
    local_tasks_.FindDict(*id)->Set(
        "expiresAt",
        (base::Time::Now() + base::Minutes(30)).InSecondsFSinceUnixEpoch());
    SetTaskState(*id, "accepted");
    if (!source_files_.contains(*id)) {
      FailTask(*id, "源文件已不可用，请重新发送");
    } else {
      ConsiderStartingFile();
    }
  } else if (*type == "reject") {
    if (sender == device_id_ && *current->FindString("status") == "pending") {
      SetTaskState(*id, "rejected");
    }
  } else {
    SetTaskState(*id, "cancelled");
  }
}

void XenonTransferService::QueueSignal(const std::string& id,
                                       std::string type) {
  const auto* task = FindInvitation(id);
  if (!task || signal_outbox_.size() >= 128) {
    return;
  }
  for (const auto [message_id, value] : signal_outbox_) {
    const auto* payload =
        value.GetDict().FindDict("message")->FindDict("payload");
    if (*payload->FindString("taskId") == id &&
        *payload->FindString("type") == type) {
      return;
    }
  }
  const auto peer = *task->FindString(
      *task->FindString("senderDeviceId") == device_id_ ? "receiverDeviceId"
                                                        : "senderDeviceId");
  base::DictValue payload;
  payload.Set("taskId", id);
  payload.Set("type", type);
  if (type == "offer") {
    payload.Set("offer",
                base::DictValue()
                    .Set("kind", *task->FindString("kind"))
                    .Set("fileName", *task->FindString("fileName"))
                    .Set("fileSize", *task->FindDouble("fileSize"))
                    .Set("createdAt", *task->FindDouble("createdAt"))
                    .Set("expiresAt", *task->FindDouble("expiresAt"))
                    .Set("preferDirect",
                         task->FindBool("preferDirect").value_or(false)));
  } else if (type == "accept") {
    payload.Set("preferDirect", task->FindBool("preferDirect").value_or(false));
  }
  const std::string message_id = NewId();
  auto message = base::DictValue()
                     .Set("id", message_id)
                     .Set("fromDeviceId", device_id_)
                     .Set("toDeviceId", peer)
                     .Set("payload", std::move(payload));
  const auto now = base::Time::Now();
  signal_outbox_.Set(
      message_id,
      base::DictValue()
          .Set("message", std::move(message))
          .Set("attempts", 0)
          .Set("nextAttemptAt", now.InSecondsFSinceUnixEpoch())
          .Set("deadline",
               (now + base::Minutes(5)).InSecondsFSinceUnixEpoch()));
  PersistLocalTasks();
  outbox_write_pending_ = true;
  const auto revision = ++outbox_write_revision_;
  profile_->GetPrefs()->CommitPendingWrite(
      base::BindOnce(&XenonTransferService::OnOutboxPersisted,
                     weak_factory_.GetWeakPtr(), generation_, revision));
}
void XenonTransferService::OnOutboxPersisted(uint64_t generation,
                                             uint64_t revision) {
  StateCallback unused;
  if (!IsCurrent(generation, unused) || revision != outbox_write_revision_) {
    return;
  }
  outbox_write_pending_ = false;
  PumpSignalOutbox();
}
void XenonTransferService::PumpSignalOutbox(bool retry_now) {
  if (account_id_.empty() || device_id_.empty() || outbox_write_pending_) {
    return;
  }
  const auto now = base::Time::Now();
  const double seconds = now.InSecondsFSinceUnixEpoch();
  std::vector<std::string> expired;
  for (auto [id, value] : signal_outbox_) {
    auto& entry = value.GetDict();
    if (*entry.FindDouble("deadline") <= seconds) {
      expired.push_back(id);
    } else if (retry_now && id != signal_in_flight_id_) {
      entry.Set("nextAttemptAt", seconds);
    }
  }
  for (const auto& id : expired) {
    signal_outbox_.Remove(id);
  }
  if (!expired.empty() || retry_now) {
    PersistLocalTasks();
  }
  if (!signal_in_flight_id_.empty()) {
    return;
  }
  signal_retry_timer_.Stop();
  std::string next;
  double earliest = std::numeric_limits<double>::infinity();
  for (const auto [id, value] : signal_outbox_) {
    const double retry = *value.GetDict().FindDouble("nextAttemptAt");
    if (retry < earliest) {
      earliest = retry;
      next = id;
    }
  }
  if (next.empty()) {
    return;
  }
  if (earliest > seconds) {
    signal_retry_timer_.Start(
        FROM_HERE, base::Seconds(earliest - seconds),
        base::BindOnce(&XenonTransferService::PumpSignalOutbox,
                       weak_factory_.GetWeakPtr(), false));
    return;
  }
  auto* entry = signal_outbox_.FindDict(next);
  const int attempts = *entry->FindInt("attempts");
  entry->Set("nextAttemptAt",
             (now + base::Seconds(kSignalRetrySeconds.at(attempts)))
                 .InSecondsFSinceUnixEpoch());
  entry->Set("attempts", std::min(attempts + 1, 5));
  auto message = entry->FindDict("message")->Clone();
  PersistLocalTasks();
  signal_in_flight_id_ = next;
  XenonAccountService::GetForProfile(profile_)->RequestForSession(
      "POST", "/signals", std::move(message),
      base::BindOnce(&XenonTransferService::OnSignalPublished,
                     weak_factory_.GetWeakPtr(), generation_, next));
}
void XenonTransferService::OnSignalPublished(uint64_t generation,
                                             std::string message_id,
                                             Response reply) {
  StateCallback unused;
  if (!IsCurrent(generation, unused) || signal_in_flight_id_ != message_id) {
    return;
  }
  signal_in_flight_id_.clear();
  const bool acknowledged = reply.status == 200 || reply.status == 201;
  const auto* entry = signal_outbox_.FindDict(message_id);
  const auto* message = entry ? entry->FindDict("message") : nullptr;
  const auto* returned = reply.body.FindDict("signal");
  bool success = acknowledged && message && returned;
  if (success) {
    for (const auto [key, value] : *message) {
      const auto* field = returned->Find(key);
      if (!field || *field != value) {
        success = false;
        break;
      }
    }
  }
  const bool retryable = acknowledged || reply.status == 0 ||
                         reply.status == 408 || reply.status == 429 ||
                         reply.status >= 500;
  if (success || !retryable) {
    if (!success) {
      const auto* payload = message ? message->FindDict("payload") : nullptr;
      if (payload && *payload->FindString("type") == "offer") {
        const auto id = *payload->FindString("taskId");
        const auto* task = FindInvitation(id);
        if (task && *task->FindString("status") == "pending") {
          FailTask(id, "邀请未能送达，请重新发送", false);
        }
      }
    }
    signal_outbox_.Remove(message_id);
    PersistLocalTasks();
  }
  PumpSignalOutbox();
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
  if (HasFileWork()) {
    Finish(std::move(callback), "请等待当前传输结束后再发送文本");
    return;
  }
  if (!IsId(receiver_id) || receiver_id == device_id_ || text.empty() ||
      text.size() > kMaxTextBytes || text.find('\0') != std::string::npos ||
      !base::IsStringUTF8(text)) {
    Finish(std::move(callback), "请选择其他设备并输入不超过 16 KB 的文本");
    return;
  }
  operation_busy_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](std::string text) {
            SourceFile source;
            source.name = kTextFileName;
            source.size = text.size();
            source.text = std::move(text);
            source.temporary_directory = NewPayloadDirectory();
            if (source.temporary_directory) {
              source.path = source.temporary_directory->GetPath().AppendASCII(
                  kTextFileName);
            }
            if (source.path.empty() ||
                !base::WriteFile(source.path, source.text)) {
              source.error = "无法准备待发送文本";
            }
            return source;
          },
          std::move(text)),
      base::BindOnce(&XenonTransferService::OnFileInspected,
                     weak_factory_.GetWeakPtr(), generation_,
                     std::move(receiver_id), std::move(callback)));
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
  CreateInvitation(std::move(receiver_id), std::move(source),
                   std::move(callback));
}

void XenonTransferService::StartJob(const base::DictValue& invitation,
                                    bool sending) {
  SetActiveFile(invitation);
  relay_pending_ = true;
  file_attempts_.insert(active_id_);
  auto request =
      XenonAccountService::GetForProfile(profile_)->GetSessionRequest();
  if (!request) {
    relay_pending_ = false;
    file_error_ = "无法建立传输连接，请重新发送";
    FailTask(active_id_, "无法建立传输连接，请重新发送");
    return;
  }
  error_.clear();
  cross_transfer::TestJobOptions options;
  options.transport = "auto";
  options.role = active_role_;
  options.task_id = active_id_;
  base::OnceClosure release_channel;
  options.automatic = ConnectionOptions(
      active_id_, device_id_,
      *invitation.FindString(sending ? "receiverDeviceId" : "senderDeviceId"),
      static_cast<uint64_t>(*invitation.FindDouble("fileSize")), sending,
      profile_->GetPrefs()->GetBoolean(xenon::prefs::kTransferUseStun),
      invitation.FindBool("preferDirect").value_or(false), direct_connections_,
      account_id_ + ":" + device_id_ + ":" +
          *invitation.FindString(sending ? "receiverDeviceId"
                                         : "senderDeviceId"),
      request, &release_channel);
  options.expected_file_name = *invitation.FindString("fileName");
  options.expected_file_size =
      static_cast<uint64_t>(*invitation.FindDouble("fileSize"));
  std::shared_ptr<base::ScopedTempDir> temporary;
  if (sending) {
    const auto source = source_files_.find(active_id_);
    if (source == source_files_.end()) {
      relay_pending_ = false;
      return;
    }
    options.source_path = source->second.path.AsUTF8Unsafe();
    temporary = source->second.temporary_directory;
  } else if (!active_text_) {
    options.output_dir = DownloadPrefs::FromBrowserContext(profile_)
                             ->DownloadPath()
                             .AppendASCII("CrossTransfer")
                             .AppendASCII(active_id_)
                             .AsUTF8Unsafe();
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](cross_transfer::TestJobOptions options,
             std::shared_ptr<base::ScopedTempDir> temporary,
             base::OnceClosure release_channel, bool text_receiver) {
            PreparedJob prepared;
            if (text_receiver) {
              temporary = NewPayloadDirectory();
              if (!temporary) {
                return prepared;
              }
              options.output_dir = temporary->GetPath().AsUTF8Unsafe();
            }
            prepared.job = std::make_shared<cross_transfer::TestTransferJob>(
                std::move(options));
            prepared.temporary_directory = std::move(temporary);
            prepared.release_channel = std::move(release_channel);
            return prepared;
          },
          std::move(options), std::move(temporary), std::move(release_channel),
          active_text_ && !sending),
      base::BindOnce(&XenonTransferService::OnJobPrepared,
                     weak_factory_.GetWeakPtr(), generation_, active_id_,
                     sending, active_text_));
}

void XenonTransferService::OnJobPrepared(uint64_t generation,
                                         std::string invitation_id,
                                         bool sending,
                                         bool text_payload,
                                         PreparedJob prepared) {
  StateCallback unused;
  if (!IsCurrent(generation, unused) || active_id_ != invitation_id) {
    return;
  }
  relay_pending_ = false;
  const auto* invitation = FindInvitation(invitation_id);
  if (!invitation || *invitation->FindString("status") != "accepted" ||
      (sending ? !source_files_.contains(invitation_id)
               : !accepted_receives_.contains(invitation_id))) {
    return;
  }
  if (!prepared.job) {
    error_ = "无法准备文本接收目录";
    file_error_ = error_;
    FailTask(invitation_id, "无法准备文本接收目录");
    return;
  }
  job_ = prepared.job;
  job_running_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](PreparedJob prepared, bool text_receiver) {
            JobResult result;
            result.success = prepared.job->Run();
            if (result.success && text_receiver) {
              const auto path = base::FilePath::FromUTF8Unsafe(
                  prepared.job->snapshot().saved_path);
              result.success = base::ReadFileToStringWithMaxSize(
                                   path, &result.text, kMaxTextBytes) &&
                               !result.text.empty() &&
                               result.text.find('\0') == std::string::npos &&
                               base::IsStringUTF8(result.text);
            }
            if (prepared.release_channel) {
              std::move(prepared.release_channel).Run();
            }
            return result;
          },
          std::move(prepared), text_payload && !sending),
      base::BindOnce(&XenonTransferService::OnJobFinished,
                     weak_factory_.GetWeakPtr(), generation_, invitation_id));
}

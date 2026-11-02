// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/account/xenon_account_service.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "cross_transfer/tls_transport_channel.h"
#include "crypto/keypair.h"

namespace {

constexpr char kUserDataKey[] = "xenon.native_account_service";
constexpr char kSessionPref[] = "xenon.account.credentials";
constexpr char kIdentityPref[] = "xenon.account.device_identity";
constexpr char kHost[] = "115.29.16.37";
constexpr size_t kMaxHeaderBytes = 8192;
// A 16 KiB text can expand sixfold when JSON-escaped, plus request metadata.
constexpr size_t kMaxBodyBytes = 128 * 1024;
// Up to 32 invitations with 16 KiB text each, including worst-case JSON
// escaping and metadata. This remains a fixed allocation bound.
constexpr size_t kMaxResponseBodyBytes = 4 * 1024 * 1024;
constexpr int kRequestTimeoutMs = 10000;

// Trust only the deployed account/relay CA. This trial CA and its server
// certificate expire on 2026-11-09 and must be renewed before then. Never fall
// back to disabling TLS verification if the service certificate is invalid.
constexpr char kAccountCaPem[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIDNTCCAh2gAwIBAgIUfP23htiaj+IcNQl6YQmIM7rolWIwDQYJKoZIhvcNAQEL
BQAwKjEoMCYGA1UEAwwfQ3Jvc3NUcmFuc2ZlciBpc29sYXRlZCB0cmlhbCBDQTAe
Fw0yNjExMDIwNjI5MTJaFw0yNjExMDkwNjI5MTJaMCoxKDAmBgNVBAMMH0Nyb3Nz
VHJhbnNmZXIgaXNvbGF0ZWQgdHJpYWwgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IB
DwAwggEKAoIBAQCa+NwCVBVByVATzARScKtiuxVxf4I0ddm4zdPmqFS8cP0c5JH1
53kzWC4bRuqnGpUL5kE7C6NCsbjKUSF/teuxcUxScp/psvlu4jabmVvTxI89xXdH
jx+BUJD/z5mxIjOB7DQHapFgtTOaeFKalz4e6cjZEq7gFT8Qu/vglBmHPUwW5FO/
SqnFqRUphz/4Fg8d1D0bOpZacrnFcnsXv9+008KrNcFzDUbIx10MoECLpeN3BxPi
cDcUWjIlJaFuxbeaUPFvSgS3eH6rE+gWC9fY0MC9TS/H2CeQrphu/S7m/t+HpB7z
NFRVSy4gQbP57O5OqDIsb1ZqlRaXzuntmhfNAgMBAAGjUzBRMB0GA1UdDgQWBBRZ
AtJX3cRfeT7pch/9eORiWQZ9fjAfBgNVHSMEGDAWgBRZAtJX3cRfeT7pch/9eORi
WQZ9fjAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAMzSYUlMcc
QXi8ZPFfbNEQe7Xtqdz45izn1vwxhGTO/utNcYRSUrrwjBzivWCERxnoOJ50/eP0
KqRrc+vi2A0QabS25SKS8Gz+F8MzRt/SteGZwWw6cERuzavOKDWSGdqSaANXZYxy
7wMfo6EjRvM9tN4xGH3sG02gX1xJ31NP0jbiNCo2XTrKNj4Y1I7yXlKSdAQZ9Iep
Ph65p7qHgahCUZqpKJ3904yrDZ2XM6zNs2S8nD2UAy4zLyoTjWdC2RkeuezXgpxl
72pjZzgTTrE9WYhMoHdRvKsE113g2Zy2UiFxuja0KL+WqrfXg/eNsQ/j0QGouGVH
KeIwHkkeLE2W
-----END CERTIFICATE-----
)PEM";

using Response = XenonAccountService::Response;
using RequestHandler = XenonAccountService::RequestHandlerForTesting;

RequestHandler& TestRequestHandler() {
  static base::NoDestructor<RequestHandler> handler;
  return *handler;
}

bool IsLowerHex(std::string_view value, size_t length) {
  return value.size() == length && std::ranges::all_of(value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool IsUsername(std::string_view value) {
  return value.size() >= 3 && value.size() <= 64 &&
         std::ranges::all_of(value, [](char c) {
           return base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) || c == '_' ||
                  c == '.' || c == '-';
         });
}

bool ReadAccount(const base::DictValue& body,
                 std::string* id,
                 std::string* username) {
  const auto* account = body.FindDict("account");
  if (!account) {
    return false;
  }
  const auto* account_id = account->FindString("id");
  const auto* account_username = account->FindString("username");
  if (!account_id || !IsLowerHex(*account_id, 32) || !account_username ||
      !IsUsername(*account_username)) {
    return false;
  }
  *id = *account_id;
  *username = *account_username;
  return true;
}

std::string ReplyError(const Response& reply) {
  if (!reply.error.empty()) {
    return reply.error;
  }
  const auto* code = reply.body.FindStringByDottedPath("error.code");
  if (code) {
    if (*code == "invalid_credentials") {
      return "用户名或密码不正确";
    }
    if (*code == "username_taken") {
      return "该用户名已被注册";
    }
    if (*code == "device_conflict") {
      return "设备登记冲突，请退出账号后重试";
    }
    if (*code == "limit_exceeded") {
      return "账号设备数量已达到上限";
    }
    if (*code == "invalid_request") {
      return "请求参数不符合账号服务要求";
    }
  }
  if (reply.status == 401) {
    return "登录已失效，请重新登录";
  }
  if (reply.status == 429) {
    return "操作过于频繁，请稍后重试";
  }
  return "账号服务暂时不可用，请稍后重试";
}

Response RequestHttps(std::string method,
                      std::string path,
                      std::string token,
                      base::DictValue body,
                      std::shared_ptr<std::atomic<bool>> cancelled,
                      RequestHandler test_handler) {
  if (cancelled->load(std::memory_order_relaxed)) {
    return {0, {}, "请求已取消"};
  }
  // base::Value represents large file sizes as doubles. Encode integral values
  // as JSON integers to match the account API's exact byte-count validation.
  auto json = base::WriteJsonWithOptions(
      body, base::JSONWriter::OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION);
  if (!json || json->size() > kMaxBodyBytes) {
    return {0, {}, "账号请求无效"};
  }
  if (test_handler) {
    return test_handler.Run(std::move(method), std::move(path),
                            std::move(token), std::move(body));
  }
  cross_transfer::TlsConfig config;
  config.ca_pem = kAccountCaPem;
  config.peer_name = kHost;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kRequestTimeoutMs);
  cross_transfer::TransportError transport_error;
  auto stream = cross_transfer::internal::TlsStream::Connect(
      kHost, 443, config, kRequestTimeoutMs, cancelled.get(), &transport_error);
  if (!stream) {
    return {0, {}, "无法安全连接账号服务，请检查网络或服务证书"};
  }
  const bool has_body = method == "POST" || method == "PUT";
  std::string request = method + " /api/v1" + path +
                        " HTTP/1.1\r\nHost: " + kHost +
                        "\r\nAccept: application/json\r\nConnection: close\r\n";
  if (!token.empty()) {
    request += "Authorization: Bearer " + token + "\r\n";
  }
  if (has_body) {
    request += "Content-Type: application/json\r\nContent-Length: " +
               base::NumberToString(json->size()) + "\r\n";
  }
  request += "\r\n";
  if (has_body) {
    request += *json;
  }
  const auto request_bytes = base::as_byte_span(request);
  if (!stream->Write(std::span<const uint8_t>(request_bytes), deadline)) {
    return {0, {}, "账号服务连接中断，请重试"};
  }

  // This endpoint emits fixed-length JSON responses. Deliberately reject
  // redirects, chunked framing, ambiguous lengths, and oversized responses.
  std::string headers;
  while (!headers.ends_with("\r\n\r\n")) {
    if (headers.size() >= kMaxHeaderBytes) {
      return {0, {}, "账号服务响应过大"};
    }
    std::array<uint8_t, 1> byte;
    if (!stream->Read(byte, deadline)) {
      return {0, {}, "账号服务响应中断，请重试"};
    }
    headers.push_back(static_cast<char>(byte.front()));
  }
  const auto lines = base::SplitStringPieceUsingSubstr(
      headers, "\r\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (lines.empty()) {
    return {0, {}, "账号服务响应格式错误"};
  }
  const auto status_parts = base::SplitStringPiece(
      lines.front(), " ", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  int status = 0;
  if (status_parts.size() < 2 ||
      (status_parts[0] != "HTTP/1.0" && status_parts[0] != "HTTP/1.1") ||
      status_parts[1].size() != 3 ||
      !base::StringToInt(status_parts[1], &status) || status < 200 ||
      status >= 600) {
    return {0, {}, "账号服务响应格式错误"};
  }
  std::optional<size_t> content_length;
  bool json_content_type = false;
  for (auto line : base::span(lines).subspan<1>()) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      return {0, {}, "账号服务响应格式错误"};
    }
    const auto name = base::ToLowerASCII(line.substr(0, colon));
    const auto value =
        base::TrimWhitespaceASCII(line.substr(colon + 1), base::TRIM_ALL);
    if (name == "content-length") {
      size_t parsed_length = 0;
      if (content_length || !base::StringToSizeT(value, &parsed_length) ||
          parsed_length == 0 || parsed_length > kMaxResponseBodyBytes) {
        return {0, {}, "账号服务响应长度无效"};
      }
      content_length = parsed_length;
    } else if (name == "transfer-encoding" || name == "content-encoding") {
      return {0, {}, "账号服务响应格式不受支持"};
    } else if (name == "content-type") {
      const auto mime = base::TrimWhitespaceASCII(
          value.substr(0, value.find(';')), base::TRIM_ALL);
      json_content_type =
          base::EqualsCaseInsensitiveASCII(mime, "application/json");
    }
  }
  if (!content_length || !json_content_type ||
      (status >= 300 && status < 400)) {
    return {0, {}, "账号服务响应格式错误"};
  }
  std::vector<uint8_t> response_bytes(*content_length);
  if (!stream->Read(response_bytes, deadline)) {
    return {0, {}, "账号服务响应中断，请重试"};
  }
  std::string response_text(response_bytes.begin(), response_bytes.end());
  auto response =
      base::JSONReader::ReadDict(response_text, base::JSON_PARSE_RFC, 16);
  if (!response) {
    return {0, {}, "账号服务返回了无效数据"};
  }
  return {status, std::move(*response), {}};
}

std::string DevicePlatform() {
#if BUILDFLAG(IS_WIN)
  return "windows";
#elif BUILDFLAG(IS_MAC)
  return "macos";
#elif BUILDFLAG(IS_LINUX)
  return "linux";
#else
  return "other";
#endif
}

void RevokeSession(std::string token,
                   std::string device_id,
                   RequestHandler test_handler) {
  auto cancellation = std::make_shared<std::atomic<bool>>(false);
  base::DictValue body;
  if (!device_id.empty()) {
    body.Set("deviceId", std::move(device_id));
  }
  RequestHttps("POST", "/auth/logout", std::move(token), std::move(body),
               cancellation, std::move(test_handler));
}

}  // namespace

// static
void XenonAccountService::SetRequestHandlerForTesting(
    RequestHandlerForTesting handler) {
  TestRequestHandler() = std::move(handler);
}

// static
XenonAccountService* XenonAccountService::GetForProfile(Profile* profile) {
  profile = profile->GetOriginalProfile();
  auto* service =
      static_cast<XenonAccountService*>(profile->GetUserData(kUserDataKey));
  if (!service) {
    auto owned =
        std::unique_ptr<XenonAccountService>(new XenonAccountService(profile));
    service = owned.get();
    profile->SetUserData(kUserDataKey, std::move(owned));
    service->Initialize();
  }
  return service;
}

XenonAccountService::XenonAccountService(Profile* profile)
    : profile_(profile),
      request_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
      cancellation_(std::make_shared<std::atomic<bool>>(false)) {}

XenonAccountService::~XenonAccountService() {
  cancellation_->store(true, std::memory_order_relaxed);
}

void XenonAccountService::Initialize() {
  restoring_ = !profile_->GetPrefs()->GetString(kSessionPref).empty();
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      &XenonAccountService::OnEncryptorReady, weak_factory_.GetWeakPtr()));
}

void XenonAccountService::OnEncryptorReady(
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  encryptor_ = std::move(encryptor);
  initializing_ = false;
  const std::string encoded = profile_->GetPrefs()->GetString(kIdentityPref);
  std::string encrypted;
  std::string cleartext;
  if (!encoded.empty() && base::Base64Decode(encoded, &encrypted) &&
      encryptor_->DecryptString(encrypted, &cleartext)) {
    auto identity =
        base::JSONReader::ReadDict(cleartext, base::JSON_PARSE_RFC, 4);
    if (identity) {
      identity_ = std::move(*identity);
    }
  }
  RestoreSession();
}

bool XenonAccountService::HasAuthenticatedSession() const {
  return authenticated_ && !token_.empty() && expires_at_ > base::Time::Now();
}

std::string XenonAccountService::GetDeviceId() const {
  return HasAuthenticatedSession() && device_registered_ ? device_id_ : "";
}

std::string XenonAccountService::GetAccountId() const {
  return HasAuthenticatedSession() ? account_id_ : "";
}

cross_transfer::TlsConfig XenonAccountService::GetTlsConfig() {
  cross_transfer::TlsConfig config;
  config.ca_pem = kAccountCaPem;
  config.peer_name = kHost;
  return config;
}

base::CallbackListSubscription XenonAccountService::AddSessionChangedCallback(
    base::RepeatingClosure callback) {
  return session_changed_callbacks_.Add(std::move(callback));
}

void XenonAccountService::RequestForSession(
    std::string method,
    std::string path,
    base::DictValue body,
    base::OnceCallback<void(Response)> callback) {
  if (!HasAuthenticatedSession()) {
    std::move(callback).Run({401, {}, "请先登录账号"});
    return;
  }
  Request(
      std::move(method), std::move(path), std::move(body),
      base::BindOnce(
          [](base::WeakPtr<XenonAccountService> service, uint64_t generation,
             base::OnceCallback<void(Response)> callback, Response reply) {
            if (!service || generation != service->generation_) {
              std::move(callback).Run({0, {}, "账号会话已改变"});
              return;
            }
            service->HandleSessionError(reply);
            std::move(callback).Run(std::move(reply));
          },
          weak_factory_.GetWeakPtr(), generation_, std::move(callback)));
}

base::DictValue XenonAccountService::GetState() const {
  base::DictValue state;
  const bool authenticated = HasAuthenticatedSession();
  state.Set("authenticated", authenticated);
  state.Set("restoring", restoring_);
  state.Set("busy", busy_ || initializing_);
  state.Set("username", username_);
  state.Set("accountId", authenticated ? account_id_ : "");
  state.Set("deviceId", authenticated ? device_id_ : "");
  state.Set("devices", authenticated ? devices_.Clone() : base::ListValue());
  state.Set("error", error_);
  return state;
}

void XenonAccountService::RestoreSession() {
  const auto stored = profile_->GetPrefs()->GetString(kSessionPref);
  if (stored.empty()) {
    restoring_ = false;
    return;
  }
  auto session = base::JSONReader::ReadDict(stored, base::JSON_PARSE_RFC, 4);
  const auto* token = session ? session->FindString("token") : nullptr;
  const auto expiry = session ? session->FindDouble("expiresAt") : std::nullopt;
  if (!session || !token || !IsLowerHex(*token, 64) || !expiry ||
      !std::isfinite(*expiry) ||
      !ReadAccount(*session, &account_id_, &username_)) {
    ClearSession();
    error_ = "本地登录状态无效，请重新登录";
    return;
  }
  token_ = *token;
  expires_at_ = base::Time::FromSecondsSinceUnixEpoch(*expiry);
  if (expires_at_ <= base::Time::Now()) {
    OnExpired();
    return;
  }
  StartTimers();
  busy_ = true;
  restoring_ = true;
  ValidateSession({});
}

void XenonAccountService::Authenticate(bool registration,
                                       std::string username,
                                       std::string password,
                                       StateCallback callback) {
  if (initializing_) {
    error_ = "设备正在初始化，请稍后重试";
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  base::TrimWhitespaceASCII(username, base::TRIM_ALL, &username);
  if (!IsUsername(username) || password.size() < 12 || password.size() > 1024 ||
      password.find('\0') != std::string::npos ||
      !base::IsStringUTF8(password)) {
    error_ = "用户名需为 3–64 个字母、数字或 ._-；密码需为 12–256 个字符";
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  ClearSession();
  busy_ = true;
  error_.clear();
  base::DictValue body;
  body.Set("username", std::move(username));
  body.Set("password", std::move(password));
  Request("POST", registration ? "/auth/register" : "/auth/login",
          std::move(body),
          base::BindOnce(&XenonAccountService::OnAuthenticated,
                         weak_factory_.GetWeakPtr(), generation_,
                         std::move(callback)));
}

void XenonAccountService::OnAuthenticated(uint64_t generation,
                                          StateCallback callback,
                                          Response reply) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  if (reply.status != 200 && reply.status != 201) {
    Finish(std::move(callback), ReplyError(reply));
    return;
  }
  const auto* token = reply.body.FindString("token");
  const auto expiry = reply.body.FindDouble("expiresAt");
  if (!token || !IsLowerHex(*token, 64) || !expiry || !std::isfinite(*expiry) ||
      !ReadAccount(reply.body, &account_id_, &username_) ||
      base::Time::FromSecondsSinceUnixEpoch(*expiry) <= base::Time::Now()) {
    ClearSession();
    Finish(std::move(callback), "账号服务返回了无效登录状态");
    return;
  }
  token_ = *token;
  expires_at_ = base::Time::FromSecondsSinceUnixEpoch(*expiry);
  authenticated_ = true;
  PersistSession();
  StartTimers();
  SyncDevice(std::move(callback));
}

void XenonAccountService::ValidateSession(StateCallback callback) {
  Request("GET", "/auth/me", {},
          base::BindOnce(&XenonAccountService::OnValidated,
                         weak_factory_.GetWeakPtr(), generation_,
                         std::move(callback)));
}

void XenonAccountService::OnValidated(uint64_t generation,
                                      StateCallback callback,
                                      Response reply) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  if (HandleSessionError(reply)) {
    Finish(std::move(callback), ReplyError(reply));
    return;
  }
  if (reply.status != 200) {
    // Keep the stored session for a later retry, but do not authenticate an
    // unvalidated restored session merely because a token exists on disk.
    Finish(std::move(callback), ReplyError(reply));
    return;
  }
  std::string id;
  std::string username;
  if (!ReadAccount(reply.body, &id, &username) || id != account_id_) {
    ClearSession();
    Finish(std::move(callback), "登录状态校验失败，请重新登录");
    return;
  }
  username_ = std::move(username);
  authenticated_ = true;
  restoring_ = false;
  SyncDevice(std::move(callback));
}

bool XenonAccountService::EnsureDeviceIdentity() {
  if (!device_id_.empty() && !public_key_.empty()) {
    return true;
  }
  const auto* owner = identity_.FindString("accountId");
  const auto* id = identity_.FindString("deviceId");
  const auto* encoded_seed = identity_.FindString("seed");
  auto seed = encoded_seed ? base::Base64Decode(*encoded_seed) : std::nullopt;
  if (owner && *owner == account_id_ && id && IsLowerHex(*id, 32) && seed &&
      seed->size() == 32) {
    auto key = crypto::keypair::PrivateKey::FromEd25519PrivateKey(
        base::span(*seed).first<32>());
    device_id_ = *id;
    public_key_ = base::Base64Encode(key.ToEd25519PublicKey());
    return true;
  }
  if (!encryptor_ || !encryptor_->IsEncryptionAvailable()) {
    error_ = "设备密钥存储暂不可用，请稍后刷新";
    return false;
  }
  std::array<uint8_t, 16> random_id;
  base::RandBytes(random_id);
  auto key = crypto::keypair::PrivateKey::GenerateEd25519();
  base::DictValue identity;
  identity.Set("accountId", account_id_);
  identity.Set("deviceId", base::HexEncodeLower(random_id));
  identity.Set("seed", base::Base64Encode(key.ToEd25519PrivateKey()));
  const auto json = base::WriteJson(identity);
  std::string encrypted;
  if (!json || !encryptor_->EncryptString(*json, &encrypted)) {
    error_ = "无法保存设备密钥，请稍后刷新";
    return false;
  }
  profile_->GetPrefs()->SetString(kIdentityPref, base::Base64Encode(encrypted));
  identity_ = std::move(identity);
  device_id_ = *identity_.FindString("deviceId");
  public_key_ = base::Base64Encode(key.ToEd25519PublicKey());
  return true;
}

void XenonAccountService::SyncDevice(StateCallback callback) {
  if (!EnsureDeviceIdentity()) {
    Finish(std::move(callback), error_);
    return;
  }
  base::DictValue body;
  if (!device_registered_) {
    body.Set("displayName", base::SysInfo::OperatingSystemName() + " · " +
                                device_id_.substr(0, 6));
    body.Set("platform", DevicePlatform());
    body.Set("publicKey", public_key_);
  }
  Request(device_registered_ ? "POST" : "PUT",
          "/devices/" + device_id_ + (device_registered_ ? "/heartbeat" : ""),
          std::move(body),
          base::BindOnce(&XenonAccountService::OnDeviceSynced,
                         weak_factory_.GetWeakPtr(), generation_,
                         !device_registered_, std::move(callback)));
}

void XenonAccountService::OnDeviceSynced(uint64_t generation,
                                         bool was_registration,
                                         StateCallback callback,
                                         Response reply) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  if (HandleSessionError(reply)) {
    Finish(std::move(callback), ReplyError(reply));
    return;
  }
  if (reply.status == 404 && !was_registration) {
    device_registered_ = false;
    SyncDevice(std::move(callback));
    return;
  }
  if (reply.status != 200 && reply.status != 201) {
    Finish(std::move(callback), ReplyError(reply));
    return;
  }
  const auto* returned_id = reply.body.FindStringByDottedPath("device.id");
  if (!returned_id || *returned_id != device_id_) {
    Finish(std::move(callback), "账号服务返回了无效设备状态");
    return;
  }
  device_registered_ = true;
  if (was_registration) {
    session_changed_callbacks_.Notify();
  }
  Request("GET", "/devices", {},
          base::BindOnce(&XenonAccountService::OnDevicesListed,
                         weak_factory_.GetWeakPtr(), generation_,
                         std::move(callback)));
}

void XenonAccountService::OnDevicesListed(uint64_t generation,
                                          StateCallback callback,
                                          Response reply) {
  if (!IsCurrent(generation, callback)) {
    return;
  }
  if (HandleSessionError(reply) || reply.status != 200) {
    Finish(std::move(callback), ReplyError(reply));
    return;
  }
  const auto* devices = reply.body.FindList("devices");
  if (!devices || devices->size() > 32) {
    Finish(std::move(callback), "账号服务返回了无效设备列表");
    return;
  }
  base::ListValue sanitized;
  for (const auto& item : *devices) {
    const auto* device = item.GetIfDict();
    const auto* id = device ? device->FindString("id") : nullptr;
    const auto* name = device ? device->FindString("displayName") : nullptr;
    const auto* platform = device ? device->FindString("platform") : nullptr;
    const auto online = device ? device->FindBool("online") : std::nullopt;
    const auto seen = device ? device->FindDouble("lastSeenAt") : std::nullopt;
    if (!id || !IsLowerHex(*id, 32) || !name || name->empty() ||
        name->size() > 512 || !platform || platform->size() > 32 || !online ||
        !seen || !std::isfinite(*seen)) {
      Finish(std::move(callback), "账号服务返回了无效设备列表");
      return;
    }
    base::DictValue safe;
    safe.Set("id", *id);
    safe.Set("displayName", *name);
    safe.Set("platform", *platform);
    safe.Set("online", *online);
    safe.Set("lastSeenAt", *seen);
    safe.Set("current", *id == device_id_);
    sanitized.Append(std::move(safe));
  }
  devices_ = std::move(sanitized);
  Finish(std::move(callback));
}

void XenonAccountService::Request(std::string method,
                                  std::string path,
                                  base::DictValue body,
                                  base::OnceCallback<void(Response)> callback) {
  request_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&RequestHttps, std::move(method), std::move(path), token_,
                     std::move(body), cancellation_, TestRequestHandler()),
      std::move(callback));
}

void XenonAccountService::Refresh(StateCallback callback) {
  if (initializing_ || busy_ || token_.empty()) {
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  if (expires_at_ <= base::Time::Now()) {
    OnExpired();
    if (callback) {
      std::move(callback).Run(GetState());
    }
    return;
  }
  busy_ = true;
  if (!authenticated_) {
    restoring_ = true;
    ValidateSession(std::move(callback));
  } else {
    SyncDevice(std::move(callback));
  }
}

void XenonAccountService::Logout(StateCallback callback) {
  std::string old_token = token_;
  std::string old_device = device_id_;
  ClearSession();
  error_.clear();
  if (!old_token.empty()) {
    // All requests share a sequence, so this revocation completes before a
    // subsequent login can re-register the same persisted device identity.
    request_runner_->PostTask(
        FROM_HERE, base::BindOnce(&RevokeSession, std::move(old_token),
                                  std::move(old_device), TestRequestHandler()));
  }
  if (callback) {
    std::move(callback).Run(GetState());
  }
}

void XenonAccountService::PersistSession() {
  base::DictValue account;
  account.Set("id", account_id_);
  account.Set("username", username_);
  base::DictValue session;
  session.Set("account", std::move(account));
  session.Set("token", token_);
  session.Set("expiresAt", expires_at_.InSecondsFSinceUnixEpoch());
  // This profile preference intentionally stores the session token directly;
  // the user requested no local token encryption. Passwords are never saved.
  profile_->GetPrefs()->SetString(kSessionPref, *base::WriteJson(session));
}

void XenonAccountService::StartTimers() {
  heartbeat_timer_.Start(FROM_HERE, base::Seconds(30), this,
                         &XenonAccountService::OnTimer);
  expiry_timer_.Start(FROM_HERE, expires_at_ - base::Time::Now(), this,
                      &XenonAccountService::OnExpired);
}

void XenonAccountService::OnTimer() {
  Refresh({});
}

void XenonAccountService::OnExpired() {
  ClearSession();
  error_ = "登录已过期，请重新登录";
}

void XenonAccountService::InvalidatePendingRequests() {
  cancellation_->store(true, std::memory_order_relaxed);
  cancellation_ = std::make_shared<std::atomic<bool>>(false);
  ++generation_;
}

void XenonAccountService::ClearSession() {
  InvalidatePendingRequests();
  heartbeat_timer_.Stop();
  expiry_timer_.Stop();
  authenticated_ = false;
  restoring_ = false;
  busy_ = false;
  device_registered_ = false;
  username_.clear();
  account_id_.clear();
  token_.clear();
  expires_at_ = base::Time();
  device_id_.clear();
  public_key_.clear();
  devices_.clear();
  profile_->GetPrefs()->ClearPref(kSessionPref);
  session_changed_callbacks_.Notify();
}

void XenonAccountService::Finish(StateCallback callback, std::string error) {
  busy_ = false;
  restoring_ = false;
  error_ = std::move(error);
  if (callback) {
    std::move(callback).Run(GetState());
  }
}

bool XenonAccountService::IsCurrent(uint64_t generation,
                                    StateCallback& callback) {
  if (generation == generation_) {
    return true;
  }
  if (callback) {
    std::move(callback).Run(GetState());
  }
  return false;
}

bool XenonAccountService::HandleSessionError(const Response& reply) {
  if (reply.status == 401) {
    ClearSession();
    return true;
  }
  return false;
}

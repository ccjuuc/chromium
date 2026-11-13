// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_safe_storage.h"

#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "build/build_config.h"
#include "xenon_overlay/buildflags/buildflags.h"

namespace xenon::ipc {
namespace {

constexpr char kUnavailable[] =
    "ERR_ENCRYPTION_UNAVAILABLE: safeStorage encryption is unavailable";
constexpr char kAborted[] = "ERR_ABORTED: safeStorage owner has exited";

bool IsAsyncMethod(const std::string& method) {
  return method == "isAsyncEncryptionAvailable" ||
         method == "encryptStringAsync" || method == "decryptStringAsync";
}

}  // namespace

ElectronSafeStorage::ElectronSafeStorage(EncryptorLoader loader)
    : loader_(std::move(loader)) {}

ElectronSafeStorage::~ElectronSafeStorage() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  weak_factory_.InvalidateWeakPtrs();
  auto pending = std::move(pending_);
  for (auto& call : pending) {
    std::move(call.callback).Run(base::unexpected(kAborted));
  }
}

void ElectronSafeStorage::Initialize() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (requested_) {
    return;
  }
  requested_ = true;
  if (!loader_) {
    OnReady(nullptr);
    return;
  }
  loader_.Run(base::BindOnce(&ElectronSafeStorage::OnReady,
                             weak_factory_.GetWeakPtr()));
}

ElectronSafeStorage::Result ElectronSafeStorage::Call(
    const std::string& owner,
    const base::Value& request) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (owner.empty()) {
    return base::unexpected("ERR_ACCESS_DENIED: missing safeStorage owner");
  }
  const auto* args = request.GetIfDict();
  const auto* method = args ? args->FindString("method") : nullptr;
  if (!method) {
    return base::unexpected("ERR_INVALID_ARG_TYPE: missing safeStorage method");
  }
  if (*method == "setUsePlainTextEncryption") {
    auto allow = args->FindBool("allow");
    if (!allow) {
      return base::unexpected(
          "ERR_INVALID_ARG_TYPE: usePlainText must be a boolean");
    }
    if (*allow) {
      allow_plain_text_.insert(owner);
    } else {
      allow_plain_text_.erase(owner);
    }
    return base::Value();
  }
  if (*method != "isEncryptionAvailable" &&
      *method != "isAsyncEncryptionAvailable" &&
      *method != "getSelectedStorageBackend" && *method != "encryptString" &&
      *method != "decryptString" && *method != "encryptStringAsync" &&
      *method != "decryptStringAsync") {
    return base::unexpected("ERR_NOT_SUPPORTED: unknown safeStorage method");
  }

  Initialize();
  if (*method == "isEncryptionAvailable" ||
      *method == "isAsyncEncryptionAvailable") {
    return base::Value(IsAvailable(owner));
  }
  if (*method == "getSelectedStorageBackend") {
#if BUILDFLAG(IS_LINUX)
    // Chromium exposes the chosen encryption provider, but does not expose
    // which service backs the Freedesktop provider (KWallet/GNOME/KeePassXC).
    // Returning unknown is deliberate; desktop environment is not evidence.
    return base::Value(IsBasicTextBackend() ? "basic_text" : "unknown");
#else
    return base::unexpected(
        "ERR_NOT_SUPPORTED: storage backend names are Linux-only");
#endif
  }
  if (!initialized_) {
    return base::unexpected(
        "ERR_NOT_READY: safeStorage initialization is pending");
  }
  if (!IsAvailable(owner)) {
    return base::unexpected(kUnavailable);
  }
  if (*method == "encryptString" || *method == "encryptStringAsync") {
    const auto* text = args->FindString("text");
    if (!text) {
      return base::unexpected(
          "ERR_INVALID_ARG_TYPE: plaintext must be a string");
    }
    auto encrypted = encryptor_->EncryptString(*text);
    if (!encrypted) {
      return base::unexpected(
          "ERR_ENCRYPTION_FAILED: safeStorage encryption failed");
    }
    return base::Value(base::Base64Encode(*encrypted));
  }

  const auto* encoded = args->FindString("ciphertext");
  std::string ciphertext;
  if (!encoded || !base::Base64Decode(*encoded, &ciphertext)) {
    return base::unexpected(
        "ERR_INVALID_ARG_TYPE: ciphertext must be a Buffer");
  }
  os_crypt_async::Encryptor::DecryptFlags flags;
  auto decrypted =
      encryptor_->DecryptData(base::as_byte_span(ciphertext), &flags);
  if (!decrypted) {
    return base::unexpected(flags.temporarily_unavailable
                                ? "ERR_ENCRYPTION_UNAVAILABLE: safeStorage "
                                  "decryption is temporarily unavailable"
                                : "ERR_DECRYPTION_FAILED: safeStorage "
                                  "ciphertext could not be decrypted");
  }
  if (*method == "decryptStringAsync") {
    return base::Value(base::DictValue()
                           .Set("result", std::move(*decrypted))
                           .Set("shouldReEncrypt", flags.should_reencrypt));
  }
  return base::Value(std::move(*decrypted));
}

void ElectronSafeStorage::CallAsync(const std::string& owner,
                                    base::Value request,
                                    Callback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto* args = request.GetIfDict();
  const auto* method = args ? args->FindString("method") : nullptr;
  if (owner.empty() || !method || !IsAsyncMethod(*method)) {
    std::move(callback).Run(base::unexpected(
        "ERR_INVALID_ARG_VALUE: invalid asynchronous safeStorage request"));
    return;
  }
  if (!initialized_) {
    pending_.push_back({owner, std::move(request), std::move(callback)});
    Initialize();
    return;
  }
  std::move(callback).Run(Call(owner, request));
}

void ElectronSafeStorage::ReleaseOwner(const std::string& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  allow_plain_text_.erase(owner);
  std::vector<Callback> cancelled;
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->owner == owner) {
      cancelled.push_back(std::move(it->callback));
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& callback : cancelled) {
    std::move(callback).Run(base::unexpected(kAborted));
  }
}

void ElectronSafeStorage::OnReady(
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  encryptor_ = std::move(encryptor);
  initialized_ = true;
  // Leave other owners' pending calls cancellable while delivering a reply:
  // that reply can synchronously retire an owner or destroy this broker.
  auto alive = weak_factory_.GetWeakPtr();
  while (!pending_.empty()) {
    Pending call = std::move(pending_.front());
    pending_.pop_front();
    Result result = Call(call.owner, call.request);
    std::move(call.callback).Run(std::move(result));
    if (!alive) {
      return;
    }
  }
}

bool ElectronSafeStorage::IsBasicTextBackend() const {
#if BUILDFLAG(IS_LINUX) && BUILDFLAG(ENABLE_XENON_SERVICE)
  // PosixKeyProvider identifies its public, fixed-key fallback as v10. Read
  // the actual selected provider; do not infer it from ciphertext or flags.
  return initialized_ && encryptor_ &&
         encryptor_->GetEncryptionProviderName() == "v10";
#else
  return false;
#endif
}

bool ElectronSafeStorage::IsAvailable(const std::string& owner) const {
  if (!initialized_ || !encryptor_ || !encryptor_->IsEncryptionAvailable()) {
    return false;
  }
  return !IsBasicTextBackend() || allow_plain_text_.contains(owner);
}

}  // namespace xenon::ipc

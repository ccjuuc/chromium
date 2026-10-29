// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_ssl_private_key.h"

#include <atomic>
#include <limits>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/bind_post_task.h"
#include "net/cert/x509_util.h"
#include "net/gm/gm_client_cert_constraints.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/sm2.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "third_party/boringssl/src/include/openssl/x509.h"

namespace net::gm {
namespace {

constexpr size_t kMaxPendingSignatures = 16;
// TLS 1.3 CertificateVerify inputs are far smaller; bound accidental misuse of
// a device-backed TLS key as a general-purpose message signer.
constexpr size_t kMaxSigningInput = 1024;
std::atomic<uint64_t> g_next_request{1};

uint64_t NewRequestId() {
  uint64_t value = g_next_request.load(std::memory_order_relaxed);
  while (value != std::numeric_limits<uint64_t>::max()) {
    if (g_next_request.compare_exchange_weak(value, value + 1,
                                             std::memory_order_relaxed)) {
      return value;
    }
  }
  return 0;
}

}  // namespace

scoped_refptr<GmSSLPrivateKey> GmSSLPrivateKey::Create(
    scoped_refptr<ClientIdentity> identity) {
  if (!identity || !identity->is_valid() ||
      !base::SingleThreadTaskRunner::HasCurrentDefault()) {
    return nullptr;
  }
  ClientCertificateRequest request;
  request.protocol = TlsProtocol::kRfc8998;
  request.signature_algorithms = {SSL_SIGN_SM2_SM3};
  if (!MatchesClientCertificateRequest(identity->signing_certificate(), {},
                                       identity->intermediates(), request)) {
    return nullptr;
  }
  auto buffer = x509_util::CreateCryptoBuffer(identity->signing_certificate());
  bssl::UniquePtr<X509> certificate(X509_parse_from_buffer(buffer.get()));
  bssl::UniquePtr<EVP_PKEY> public_key(
      certificate ? X509_get_pubkey(certificate.get()) : nullptr);
  const EC_KEY* ec =
      public_key ? EVP_PKEY_get0_EC_KEY(public_key.get()) : nullptr;
  if (!ec || EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)) != NID_sm2 ||
      !EC_KEY_check_key(ec)) {
    return nullptr;
  }
  return base::WrapRefCounted(
      new GmSSLPrivateKey(std::move(identity), std::move(public_key)));
}

GmSSLPrivateKey::GmSSLPrivateKey(scoped_refptr<ClientIdentity> identity,
                                 bssl::UniquePtr<EVP_PKEY> public_key)
    : identity_(std::move(identity)),
      public_key_(std::move(public_key)),
      task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {
  invalidation_subscription_ =
      identity_->AddInvalidationCallback(base::BindRepeating(
          &GmSSLPrivateKey::OnInvalidated, weak_factory_.GetWeakPtr()));
}

GmSSLPrivateKey::~GmSSLPrivateKey() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  weak_factory_.InvalidateWeakPtrs();
  invalidation_subscription_ = {};
  CancelRequests(ERR_ABORTED, false);
}

std::string GmSSLPrivateKey::GetProviderName() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return "SM2 USBKey";
}

std::vector<uint16_t> GmSSLPrivateKey::GetAlgorithmPreferences() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return {SSL_SIGN_SM2_SM3};
}

void GmSSLPrivateKey::Reply(SignCallback callback,
                            Error error,
                            std::vector<uint8_t> signature) {
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<GmSSLPrivateKey> key, SignCallback completion,
             Error result, std::vector<uint8_t> output) {
            if (key) {
              std::move(completion).Run(result, output);
            }
          },
          weak_factory_.GetWeakPtr(), std::move(callback), error,
          std::move(signature)));
}

void GmSSLPrivateKey::Sign(uint16_t algorithm,
                           base::span<const uint8_t> input,
                           SignCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!identity_->is_valid()) {
    Reply(std::move(callback), ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return;
  }
  if (cancelled_) {
    Reply(std::move(callback), ERR_ABORTED);
    return;
  }
  if (algorithm != SSL_SIGN_SM2_SM3 || input.empty() ||
      input.size() > kMaxSigningInput) {
    Reply(std::move(callback), ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED);
    return;
  }
  if (pending_.size() >= kMaxPendingSignatures) {
    Reply(std::move(callback), ERR_INSUFFICIENT_RESOURCES);
    return;
  }
  const uint64_t request_id = NewRequestId();
  if (!request_id) {
    Reply(std::move(callback), ERR_INSUFFICIENT_RESOURCES);
    return;
  }
  PendingSignature pending;
  constexpr uint8_t kIdentity[] = "TLSv1.3+GM+Cipher+Suite";
  if (!SM2_compute_digest(pending.digest.data(),
                          EVP_PKEY_get0_EC_KEY(public_key_.get()), kIdentity,
                          sizeof(kIdentity) - 1, input.data(), input.size())) {
    Reply(std::move(callback), ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED);
    return;
  }
  pending.callback = std::move(callback);
  auto [position, inserted] = pending_.emplace(request_id, std::move(pending));
  CHECK(inserted);
  OPENSSL_cleanse(pending.digest.data(), pending.digest.size());
  identity_->SignDigest(
      request_id, position->second.digest,
      base::BindPostTask(
          task_runner_,
          base::BindOnce(&GmSSLPrivateKey::OnSignature,
                         weak_factory_.GetWeakPtr(), request_id)));
}

void GmSSLPrivateKey::OnSignature(uint64_t request_id,
                                  int error,
                                  std::vector<uint8_t> signature) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto found = pending_.find(request_id);
  if (found == pending_.end()) {
    return;
  }
  Error result = OK;
  if (!identity_->is_valid()) {
    result = ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
  } else if (cancelled_) {
    result = ERR_ABORTED;
  } else if (error != OK) {
    result = error < 0 && error != ERR_IO_PENDING
                 ? static_cast<Error>(error)
                 : ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED;
  } else if (signature.empty() || signature.size() > 72 ||
             !SM2_verify_digest(found->second.digest.data(), signature.data(),
                                signature.size(),
                                EVP_PKEY_get0_EC_KEY(public_key_.get()))) {
    result = ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED;
  }
  auto callback = std::move(found->second.callback);
  OPENSSL_cleanse(found->second.digest.data(), found->second.digest.size());
  pending_.erase(found);
  if (result != OK) {
    signature.clear();
  }
  // Device delivery has already been posted to this thread. Complete here so
  // a later cancellation cannot overtake a successful result awaiting another
  // task. No member may be accessed after the callback destroys this adapter.
  std::move(callback).Run(result, signature);
}

void GmSSLPrivateKey::CancelRequests(Error error, bool notify) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  cancelled_ = true;
  // Keep callbacks for operations which have already failed and queued a
  // reply. Device completions are rejected by removing their request IDs;
  // only destruction invalidates all queued delivery callbacks.
  auto requests = std::exchange(pending_, {});
  for (auto& [request_id, pending] : requests) {
    OPENSSL_cleanse(pending.digest.data(), pending.digest.size());
    identity_->Cancel(request_id);
    if (notify) {
      Reply(std::move(pending.callback), error);
    }
  }
}

void GmSSLPrivateKey::Cancel() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (cancelled_) {
    return;
  }
  scoped_refptr<GmSSLPrivateKey> keep_alive(this);
  CancelRequests(ERR_ABORTED, true);
}

void GmSSLPrivateKey::OnInvalidated() {
  scoped_refptr<GmSSLPrivateKey> keep_alive(this);
  CancelRequests(ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED, true);
}

}  // namespace net::gm

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_client_identity.h"

#include <utility>

#include "base/rand_util.h"

namespace net::gm {

scoped_refptr<ClientIdentity> ClientIdentityProvider::GetCachedIdentity(
    const HostPortPair&,
    const ConnectionProfile&) {
  return nullptr;
}

ClientIdentity::ClientIdentity(std::string identity_id,
                               uint64_t generation,
                               std::vector<uint8_t> signing_certificate,
                               std::vector<uint8_t> encryption_certificate,
                               std::vector<std::vector<uint8_t>> intermediates,
                               uint64_t signing_key_id,
                               uint64_t encryption_key_id)
    : identity_id_(std::move(identity_id)),
      generation_(generation),
      signing_key_id_(signing_key_id ? signing_key_id : base::RandUint64() | 1),
      encryption_key_id_(encryption_key_id ? encryption_key_id
                                           : base::RandUint64() | 1),
      signing_certificate_(std::move(signing_certificate)),
      encryption_certificate_(std::move(encryption_certificate)),
      intermediates_(std::move(intermediates)) {}

ClientIdentity::~ClientIdentity() = default;

base::CallbackListSubscription ClientIdentity::AddInvalidationCallback(
    base::RepeatingClosure callback) {
  return invalidation_callbacks_.Add(std::move(callback));
}

void ClientIdentity::Invalidate() {
  if (std::exchange(valid_, false)) {
    // A callback may release the last external reference when it cancels its
    // connection. The callback list must remain alive throughout notification.
    scoped_refptr<ClientIdentity> keep_alive(this);
    invalidation_callbacks_.Notify();
  }
}

}  // namespace net::gm

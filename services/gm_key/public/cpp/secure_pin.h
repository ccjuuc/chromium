// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_GM_KEY_PUBLIC_CPP_SECURE_PIN_H_
#define SERVICES_GM_KEY_PUBLIC_CPP_SECURE_PIN_H_

#include <cstdint>
#include <vector>

namespace mojo {
template <typename DataViewType, typename T>
struct StructTraits;
}

namespace gm_key {

namespace mojom {
class KeyPinDataView;
}

// Owns one PIN allocation and wipes it before destruction or replacement.
// This protects owned/decoded buffers, not Mojo's internal transport buffers.
class SecurePin {
 public:
  SecurePin();
  explicit SecurePin(std::vector<uint8_t> value);
  ~SecurePin();
  SecurePin(SecurePin&& other) noexcept;
  SecurePin& operator=(SecurePin&& other) noexcept;
  SecurePin(const SecurePin&) = delete;
  SecurePin& operator=(const SecurePin&) = delete;

  const std::vector<uint8_t>& value() const { return value_; }

 private:
  friend struct mojo::StructTraits<mojom::KeyPinDataView, SecurePin>;
  // Only used while decoding into a fresh SecurePin.
  std::vector<uint8_t>* Receive();
  void Wipe();
  std::vector<uint8_t> value_;
};

}  // namespace gm_key

#endif  // SERVICES_GM_KEY_PUBLIC_CPP_SECURE_PIN_H_

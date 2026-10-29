// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/gm_key/public/cpp/secure_pin.h"

#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace gm_key {

SecurePin::SecurePin() = default;
SecurePin::SecurePin(std::vector<uint8_t> value) : value_(std::move(value)) {}
SecurePin::~SecurePin() {
  Wipe();
}
SecurePin::SecurePin(SecurePin&& other) noexcept = default;
SecurePin& SecurePin::operator=(SecurePin&& other) noexcept {
  if (this != &other) {
    Wipe();
    value_ = std::move(other.value_);
  }
  return *this;
}
std::vector<uint8_t>* SecurePin::Receive() {
  Wipe();
  return &value_;
}
void SecurePin::Wipe() {
#if defined(_WIN32)
  if (!value_.empty()) {
    SecureZeroMemory(value_.data(), value_.size());
  }
#else
  for (uint8_t& byte : value_) {
    volatile uint8_t& sensitive_byte = byte;
    sensitive_byte = 0;
  }
#endif
  value_.clear();
}

}  // namespace gm_key

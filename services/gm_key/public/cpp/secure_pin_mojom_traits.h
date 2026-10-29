// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_GM_KEY_PUBLIC_CPP_SECURE_PIN_MOJOM_TRAITS_H_
#define SERVICES_GM_KEY_PUBLIC_CPP_SECURE_PIN_MOJOM_TRAITS_H_

#include "mojo/public/cpp/bindings/struct_traits.h"
#include "services/gm_key/public/cpp/secure_pin.h"
#include "services/gm_key/public/mojom/gm_key_service.mojom-shared.h"

namespace mojo {

template <>
struct StructTraits<gm_key::mojom::KeyPinDataView, gm_key::SecurePin> {
  static const std::vector<uint8_t>& value(const gm_key::SecurePin& pin) {
    return pin.value();
  }
  static bool Read(gm_key::mojom::KeyPinDataView data, gm_key::SecurePin* out) {
    mojo::ArrayDataView<uint8_t> bytes;
    data.GetValueDataView(&bytes);
    if (bytes.size() == 0 || bytes.size() > 128) {
      return false;
    }
    if (!data.ReadValue(out->Receive())) {
      return false;
    }
    return true;
  }
};

}  // namespace mojo

#endif  // SERVICES_GM_KEY_PUBLIC_CPP_SECURE_PIN_MOJOM_TRAITS_H_

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/chromium_leaf_glue.h"

#include "net/net_buildflags.h"
#include "third_party/chromium_leaf/src/lib.rs.h"

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)

namespace net {

int ChromiumLeafFfiAbiVersion() {
  return net::chromium_leaf::chromium_leaf_ffi_abi_version();
}

}  // namespace net

#endif  // BUILDFLAG(ENABLE_CHROMIUM_LEAF)

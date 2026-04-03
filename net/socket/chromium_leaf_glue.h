// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_CHROMIUM_LEAF_GLUE_H_
#define NET_SOCKET_CHROMIUM_LEAF_GLUE_H_

#include "net/base/net_export.h"
#include "net/net_buildflags.h"

namespace net {

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
// cxx bridge: kept for optional future Rust helpers; VLESS handshake lives on
// LeafClientSocket's StreamSocket path.
NET_EXPORT_PRIVATE int ChromiumLeafFfiAbiVersion();
#endif  // BUILDFLAG(ENABLE_CHROMIUM_LEAF)

}  // namespace net

#endif  // NET_SOCKET_CHROMIUM_LEAF_GLUE_H_

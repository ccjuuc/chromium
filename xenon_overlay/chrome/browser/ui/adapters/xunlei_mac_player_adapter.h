// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_ADAPTERS_XUNLEI_MAC_PLAYER_ADAPTER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_ADAPTERS_XUNLEI_MAC_PLAYER_ADAPTER_H_

namespace xenon {
struct MacPlayerAdapter;
// SDK ABI/registration contracts for the Xunlei Mac player. Bump the adapter
// version when these contracts change, independently of the transport version.
const MacPlayerAdapter& XunleiMacPlayerAdapterV1();
}  // namespace xenon

#endif

// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_NAPI_NAPI_SWITCHES_H_
#define XENON_OVERLAY_CHROME_BROWSER_NAPI_NAPI_SWITCHES_H_

namespace xenon::napi_switches {

inline constexpr char kEnableNodeStaticRegistration[] =
    "enable-xenon-node-static-registration";
inline constexpr char kDisableNodeStaticRegistration[] =
    "disable-xenon-node-static-registration";
inline constexpr char kAllowExternalNodeAddons[] =
    "allow-external-xenon-node-addons";

}  // namespace xenon::napi_switches

#endif  // XENON_OVERLAY_CHROME_BROWSER_NAPI_NAPI_SWITCHES_H_

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_GM_KEY_COMPILER_COMPAT_H_
#define SERVICES_GM_KEY_COMPILER_COMPAT_H_

// The standalone SDK-adapter test also supports MSVC. Chromium requires Clang
// and uses its usual narrowly scoped annotations for Windows/C ABI boundaries.
#if defined(__clang__)
#include "base/compiler_specific.h"
#define GM_KEY_UNSAFE_BUFFERS(...) UNSAFE_BUFFERS(__VA_ARGS__)
#else
#define GM_KEY_UNSAFE_BUFFERS(...) __VA_ARGS__
#endif

#endif  // SERVICES_GM_KEY_COMPILER_COMPAT_H_

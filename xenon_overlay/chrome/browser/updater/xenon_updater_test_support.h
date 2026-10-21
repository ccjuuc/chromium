// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATER_TEST_SUPPORT_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATER_TEST_SUPPORT_H_

#if defined(XENON_UPDATER_STANDALONE_TESTS)
#include "mojo/core/embedder/embedder.h"
#endif

namespace xenon::updater {

#if defined(XENON_UPDATER_STANDALONE_TESTS)
int GetUpdateExitRequestsForTesting();
#endif

inline void InitializeUpdaterMojoForTesting() {
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  static const bool initialized = [] {
    mojo::core::Init();
    return true;
  }();
  (void)initialized;
#endif
  // The Chrome unit-test suite initializes Mojo before running these tests.
}

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATER_TEST_SUPPORT_H_

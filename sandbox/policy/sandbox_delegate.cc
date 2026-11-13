// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sandbox/policy/sandbox_delegate.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
namespace sandbox::policy {

bool SandboxDelegate::ApplyXenonUtilityProcessOptions(base::LaunchOptions*) {
  return true;
}

}  // namespace sandbox::policy
#endif  // BUILDFLAG(ENABLE_XENON_SERVICE)

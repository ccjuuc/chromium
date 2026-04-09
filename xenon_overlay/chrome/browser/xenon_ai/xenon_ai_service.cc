// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.h"

#include "base/check.h"
#include "chrome/browser/profiles/profile.h"

namespace xenon {

XenonAiService::XenonAiService(Profile* profile)
    : profile_(CHECK_DEREF(profile)), build_stamp_("xenon-ai-1") {}

XenonAiService::~XenonAiService() = default;

std::string XenonAiService::GetBuildStamp() const {
  return build_stamp_;
}

}  // namespace xenon

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.h"

#include "base/check_deref.h"
#include "chrome/browser/profiles/profile.h"

namespace xenon {

namespace {

std::string XenonBuildStamp() {
  return "xenon-ai-1.0-alpha";
}

}  // namespace

XenonAiService::XenonAiService(Profile* profile)
    : profile_(profile),
      build_stamp_(XenonBuildStamp()) {}

XenonAiService::~XenonAiService() = default;

void XenonAiService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void XenonAiService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

std::string XenonAiService::GetBuildStamp() const {
  return build_stamp_;
}

void XenonAiService::SetPendingPrompt(const std::string& prompt) {
  pending_prompt_ = prompt;
  for (auto& observer : observers_) {
    observer.OnPromptReceived(prompt);
  }
}

std::string XenonAiService::GetPendingPrompt() const {
  return pending_prompt_;
}

}  // namespace xenon

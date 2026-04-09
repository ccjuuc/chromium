// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace xenon {

// Browser-process domain service for Xenon AI. UI (WebUI / side panel) and
// thin page APIs should depend on this—not on each other.
class XenonAiService : public KeyedService {
 public:
  explicit XenonAiService(Profile* profile);
  XenonAiService(const XenonAiService&) = delete;
  XenonAiService& operator=(const XenonAiService&) = delete;
  ~XenonAiService() override;

  Profile* profile() const { return profile_.get(); }

  // Identity string for wiring checks; extend with session/model state later.
  std::string GetBuildStamp() const;

 private:
  const raw_ptr<Profile> profile_;
  const std::string build_stamp_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_H_

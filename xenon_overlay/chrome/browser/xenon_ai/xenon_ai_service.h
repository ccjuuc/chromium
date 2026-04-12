// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_H_

#include <string>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace xenon {

// Browser-process domain service for Xenon AI. UI (WebUI / side panel) and
// thin page APIs should depend on this—not on each other.
class XenonAiService : public KeyedService {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnPromptReceived(const std::string& prompt) {}
  };

  explicit XenonAiService(Profile* profile);
  XenonAiService(const XenonAiService&) = delete;
  XenonAiService& operator=(const XenonAiService&) = delete;
  ~XenonAiService() override;

  Profile* profile() const { return profile_.get(); }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Identity string for wiring checks; extend with session/model state later.
  std::string GetBuildStamp() const;

  // Set the prompt that should be picked up by the side panel UI.
  void SetPendingPrompt(const std::string& prompt);
  std::string GetPendingPrompt() const;

  // Runs `xenon_ai_inferencer_burn` off-thread; |callback| is always posted to the
  // browser UI thread. On failure, |success| is false and |text| holds an error
  // string.
  void RunInferencerRewriteAsync(
      const std::string& instruction_utf8,
      const std::string& selection_utf8,
      base::OnceCallback<void(bool success, const std::string& text)> callback);

 private:
  const base::raw_ptr<Profile> profile_;
  const std::string build_stamp_;
  std::string pending_prompt_;
  base::ObserverList<Observer> observers_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_H_

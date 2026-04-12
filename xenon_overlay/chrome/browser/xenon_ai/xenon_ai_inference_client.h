// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_INFERENCE_CLIENT_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_INFERENCE_CLIENT_H_

#include <string>

#include "build/build_config.h"

namespace xenon {

// Runs `xenon_ai_inferencer_burn` from DIR_EXE (Cargo build via GN action, next to
// chrome.exe). Protocol: stdout lines DELTA ... and DONE <code>. Uses a single
// `--stdio-server` child so Llama weights stay in memory between rewrites.
class XenonAiInferenceClient {
 public:
  XenonAiInferenceClient() = delete;

  // Synchronous: blocks until the child exits. Do not call from the UI thread;
  // use ThreadPool + reply (see xenon_ai_service RunInferencerRewriteAsync).
  static bool RunRewrite(const std::string& instruction_utf8,
                         const std::string& selection_utf8,
                         std::string* combined_output,
                         std::string* error_message);
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_INFERENCE_CLIENT_H_

// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_UTILITY_UTILITY_THREAD_H_
#define CONTENT_PUBLIC_UTILITY_UTILITY_THREAD_H_

#include "base/auto_reset.h"
#include "build/build_config.h"
#include "content/common/content_export.h"
#include "content/public/child/child_thread.h"
#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include "base/functional/callback_forward.h"
#endif

namespace content {

class CONTENT_EXPORT UtilityThread : virtual public ChildThread {
 public:
  // Returns the one utility thread for this process.  Note that this can only
  // be accessed when running on the utility thread itself.
  static UtilityThread* Get();

  UtilityThread();
  ~UtilityThread() override;

  // Releases the process.
  virtual void ReleaseProcess() = 0;

#if BUILDFLAG(ENABLE_XENON_SERVICE)
  // Retains an out-of-process Utility until the returned closure is run.
  // Native application cleanup may outlive its Mojo service receiver. The
  // release is posted to the owning sequence; in-process utilities return an
  // empty closure and never change the browser process lifetime.
  virtual base::OnceClosure AcquireXenonProcessKeepAlive();
#endif

  // Initializes blink if it hasn't already been initialized.
  virtual void EnsureBlinkInitialized() = 0;

#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)
  // Initializes blink with web sandbox support.
  virtual void EnsureBlinkInitializedWithSandboxSupport() = 0;
#endif

 private:
  const base::AutoReset<UtilityThread*> resetter_;
};

}  // namespace content

#endif  // CONTENT_PUBLIC_UTILITY_UTILITY_THREAD_H_

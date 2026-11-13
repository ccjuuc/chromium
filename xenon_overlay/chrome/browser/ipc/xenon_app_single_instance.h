// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_SINGLE_INSTANCE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_SINGLE_INSTANCE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/types/expected.h"
#include "base/values.h"

namespace xenon::ipc {

// One broker per Browser process. All calls, destruction and notifications use
// the same MayBlock sequence. Owners identify a container AND its generation.
// The caller supplies trusted Browser paths, never paths from application JS.
class XenonAppSingleInstanceBroker {
 public:
  struct Error {
    std::string code;
    std::string message;
  };
  struct Notification {
    base::ListValue argv;
    base::FilePath working_directory;
    base::Value additional_data;
  };
  using NotificationCallback =
      base::RepeatingCallback<void(const std::string& owner, Notification)>;

  // Only additionalData comes from JS. Launch argv and the application's
  // current effective cwd come from native runtime state, regardless
  // of changes to process.argv, process.cwd, or bootstrap globals in JS.
  static base::expected<Notification, Error> NotificationFromRequest(
      const std::vector<std::string>& launch_argv,
      const base::FilePath& executable,
      const base::FilePath& working_directory,
      const base::DictValue& request);
  // The authenticated main-process transport overwrites the runtime snapshot
  // after converting JS arguments. Never fall back to a stale startup cwd.
  static base::expected<Notification, Error> NotificationFromNativeRequest(
      const std::vector<std::string>& launch_argv,
      const base::FilePath& executable,
      const base::DictValue& request);

  explicit XenonAppSingleInstanceBroker(NotificationCallback callback);
  ~XenonAppSingleInstanceBroker();
  XenonAppSingleInstanceBroker(const XenonAppSingleInstanceBroker&) = delete;
  XenonAppSingleInstanceBroker& operator=(const XenonAppSingleInstanceBroker&) =
      delete;

  base::expected<bool, Error> Request(const std::string& owner,
                                      const base::FilePath& app_path,
                                      const base::FilePath& host_user_data,
                                      Notification notification);
  bool Has(const std::string& owner) const;
  void Release(const std::string& owner);
  void ReleaseAll();

 private:
  struct Entry;
  void Deliver(const std::string& key,
               const std::string& owner,
               const std::string& lease,
               std::string message);

  const NotificationCallback notification_callback_;
  std::map<std::string, std::unique_ptr<Entry>> entries_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<XenonAppSingleInstanceBroker> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_SINGLE_INSTANCE_H_

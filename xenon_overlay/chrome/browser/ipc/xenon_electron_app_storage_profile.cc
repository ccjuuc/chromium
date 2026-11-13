// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_storage_profile.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_service_context.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_cookie_encryption.h"

namespace xenon::ipc {
namespace {
// A self-owned builder keeps Profile's raw delegate alive throughout async
// initialization. Delivery is posted so a synchronous failure cannot delete the
// delegate while Profile::CreateProfile still uses its stack.
class ProfileBuilder final : public Profile::Delegate {
 public:
  static void Create(const base::FilePath& path,
                     ElectronAppStorageContextCreated callback) {
    auto* builder = new ProfileBuilder(path, std::move(callback));
    ElectronCookieEncryption::Create(
        path, base::BindOnce(&ProfileBuilder::OnEncryptionReady,
                             base::Unretained(builder)));
  }
  void OnProfileCreationStarted(Profile* profile,
                                Profile::CreateMode) override {
    ElectronCookieEncryption::Install(profile, std::move(encryption_));
    content::EnableDedicatedStorageServiceForBrowserContext(profile);
  }
  void OnProfileCreationFinished(Profile*,
                                 Profile::CreateMode,
                                 bool success,
                                 bool) override {
    Finish(success);
  }

 private:
  ProfileBuilder(const base::FilePath& path,
                 ElectronAppStorageContextCreated callback)
      : path_(path), callback_(std::move(callback)) {}
  void OnEncryptionReady(std::unique_ptr<ElectronCookieEncryption> encryption,
                         std::string error) {
    if (!encryption) {
      error_ = std::move(error);
      Finish(false);
      return;
    }
    encryption_ = std::move(encryption);
    profile_ =
        Profile::CreateProfile(path_, this, Profile::CreateMode::kAsynchronous);
    if (!profile_) {
      Finish(false);
    }
  }
  void Finish(bool success) {
    if (posted_) {
      return;
    }
    posted_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&ProfileBuilder::Deliver,
                                  base::Unretained(this), success));
  }
  void Deliver(bool success) {
    std::unique_ptr<ProfileBuilder> self(this);
    auto callback = std::move(callback_);
    if (!success) {
      // Keep the directory lease until a partially initialized Profile has
      // actually completed its normal destruction sequence as well.
      std::move(callback).Run(
          std::move(profile_),
          error_.empty()
              ? "ERR_FAILED: application Profile initialization failed"
              : std::move(error_));
      return;
    }
    std::move(callback).Run(std::move(profile_), "");
  }
  const base::FilePath path_;
  std::unique_ptr<ElectronCookieEncryption> encryption_;
  std::unique_ptr<Profile> profile_;
  ElectronAppStorageContextCreated callback_;
  std::string error_;
  bool posted_ = false;
};

}  // namespace

void CreateElectronAppStorageProfile(
    const base::FilePath& path,
    ElectronAppStorageContextCreated callback) {
  ProfileBuilder::Create(path, std::move(callback));
}

void DestroyElectronAppStorageProfile(
    std::unique_ptr<content::BrowserContext> context) {
  ProfileDestroyer::DestroyOriginalProfileWhenAppropriate(
      std::unique_ptr<Profile>(Profile::FromBrowserContext(context.release())));
}

}  // namespace xenon::ipc

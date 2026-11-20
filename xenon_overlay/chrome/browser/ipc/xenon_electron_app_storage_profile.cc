// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_storage_profile.h"

#include <map>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_service_context.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_cookie_encryption.h"

namespace xenon::ipc {
namespace {
class ProfileBuilder;

// ProfileImpl retains its delegate pointer until destruction, after creation
// has completed. Give all application Profiles a stable delegate and retain
// only their unfinished creation requests in the routing table.
class ProfileDelegateDispatcher final : public Profile::Delegate {
 public:
  static ProfileDelegateDispatcher* GetInstance() {
    static base::NoDestructor<ProfileDelegateDispatcher> dispatcher;
    return dispatcher.get();
  }

  std::unique_ptr<Profile> CreateProfile(const base::FilePath& path,
                                         ProfileBuilder* builder);
  void ForgetProfile(Profile* profile);

  void OnProfileCreationStarted(Profile* profile,
                                Profile::CreateMode create_mode) override;
  void OnProfileCreationFinished(Profile* profile,
                                 Profile::CreateMode create_mode,
                                 bool success,
                                 bool is_new_profile) override;

 private:
  friend class base::NoDestructor<ProfileDelegateDispatcher>;
  ProfileDelegateDispatcher() = default;
  ~ProfileDelegateDispatcher() override = default;

  raw_ptr<ProfileBuilder> creating_builder_ = nullptr;
  std::map<Profile*, raw_ptr<ProfileBuilder>> pending_profiles_;
};

// A self-owned builder lasts only through async initialization. Delivery is
// posted so a synchronous completion cannot delete the builder while
// Profile::CreateProfile still uses its stack.
class ProfileBuilder final {
 public:
  static void Create(const base::FilePath& path,
                     ElectronAppStorageContextCreated callback) {
    auto* builder = new ProfileBuilder(path, std::move(callback));
    ElectronCookieEncryption::Create(
        path, base::BindOnce(&ProfileBuilder::OnEncryptionReady,
                             base::Unretained(builder)));
  }
  void OnProfileCreationStarted(Profile* profile, Profile::CreateMode) {
    ElectronCookieEncryption::Install(profile, std::move(encryption_));
    content::EnableDedicatedStorageServiceForBrowserContext(profile);
  }
  void OnProfileCreationFinished(Profile*,
                                 Profile::CreateMode,
                                 bool success,
                                 bool) {
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
        ProfileDelegateDispatcher::GetInstance()->CreateProfile(path_, this);
    if (!profile_) {
      Finish(false);
    }
  }
  void Finish(bool success) {
    if (posted_) {
      return;
    }
    posted_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostNonNestableTask(
        FROM_HERE, base::BindOnce(&ProfileBuilder::Deliver,
                                  base::Unretained(this), success));
  }
  void Deliver(bool success) {
    std::unique_ptr<ProfileBuilder> self(this);
    if (profile_) {
      ProfileDelegateDispatcher::GetInstance()->ForgetProfile(profile_.get());
    }
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

std::unique_ptr<Profile> ProfileDelegateDispatcher::CreateProfile(
    const base::FilePath& path,
    ProfileBuilder* builder) {
  // OnProfileCreationStarted runs inside CreateProfile. Restore the previous
  // request if initialization synchronously starts another application Profile.
  base::AutoReset<raw_ptr<ProfileBuilder>> creating_builder(&creating_builder_,
                                                            builder);
  return Profile::CreateProfile(path, this, Profile::CreateMode::kAsynchronous);
}

void ProfileDelegateDispatcher::ForgetProfile(Profile* profile) {
  pending_profiles_.erase(profile);
}

void ProfileDelegateDispatcher::OnProfileCreationStarted(
    Profile* profile,
    Profile::CreateMode create_mode) {
  CHECK(creating_builder_);
  CHECK(pending_profiles_.emplace(profile, creating_builder_).second);
  creating_builder_->OnProfileCreationStarted(profile, create_mode);
}

void ProfileDelegateDispatcher::OnProfileCreationFinished(
    Profile* profile,
    Profile::CreateMode create_mode,
    bool success,
    bool is_new_profile) {
  auto found = pending_profiles_.find(profile);
  if (found != pending_profiles_.end()) {
    found->second->OnProfileCreationFinished(profile, create_mode, success,
                                             is_new_profile);
  }
}

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

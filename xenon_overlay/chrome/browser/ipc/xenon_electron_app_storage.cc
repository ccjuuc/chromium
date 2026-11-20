// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_storage.h"

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "components/keyed_service/core/keyed_service_shutdown_notifier.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition_path.h"
#include "content/public/browser/storage_service_context.h"
#include "crypto/hash.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"

namespace xenon::ipc {
namespace {

const char kContextMarker[] = "xenon-electron-application-storage";
constexpr char kRetired[] = "ERR_ABORTED: application storage owner exited";

class StorageOwner;
auto& Owners() {
  static base::NoDestructor<std::map<std::string, scoped_refptr<StorageOwner>>>
      owners;
  return *owners;
}

struct PathLess {
  bool operator()(const base::FilePath& a, const base::FilePath& b) const {
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    // Be conservative on case-sensitive macOS volumes too: conflicting aliases
    // must never open the same profile twice in one process.
    return base::FilePath::CompareIgnoreCase(a.value(), b.value()) < 0;
#else
    return a < b;
#endif
  }
};

auto& Reservations() {
  static base::NoDestructor<std::map<base::FilePath, std::string, PathLess>>
      paths;
  return *paths;
}

struct ContextHooks {
  ElectronAppStorageContextFactory create;
  ElectronAppStorageContextDestroyer destroy;
};
ContextHooks& TestHooks() {
  static base::NoDestructor<ContextHooks> hooks;
  return *hooks;
}

content::StoragePartitionConfig LegacyConfig(content::BrowserContext* context,
                                             const std::string& container,
                                             const std::string& partition) {
  return content::StoragePartitionConfig::Create(
      context, "xenon" + base::HexEncodeLower(crypto::hash::Sha256(container)),
      base::HexEncodeLower(crypto::hash::Sha256(partition)),
      !partition.empty() && !base::StartsWith(partition, "persist:"));
}

base::FilePath Normalize(const base::FilePath& path) {
  return path.NormalizePathSeparators().StripTrailingSeparators();
}
bool SamePath(const base::FilePath& a, const base::FilePath& b) {
  const auto first = Normalize(a);
  const auto second = Normalize(b);
  return !PathLess()(first, second) && !PathLess()(second, first);
}

bool SameOrParent(const base::FilePath& ancestor, base::FilePath path) {
  // FilePath::IsParent compares directory components case-sensitively, even
  // on Windows. Use the same alias rules as our directory reservation keys.
  for (;;) {
    if (SamePath(ancestor, path)) {
      return true;
    }
    auto parent = path.DirName();
    if (parent == path) {
      return false;
    }
    path = std::move(parent);
  }
}

bool OverlappingPaths(const base::FilePath& a, const base::FilePath& b) {
  return SameOrParent(a, b) || SameOrParent(b, a);
}

// Resolve existing ancestors without creating the selected directory. This
// catches files in the path and symlink aliases before committing an override.
base::expected<base::FilePath, std::string> CanonicalDirectory(
    const base::FilePath& requested) {
  if (!requested.IsAbsolute()) {
    return base::unexpected("ERR_INVALID_ARG_VALUE: Path must be absolute");
  }
  base::FilePath ancestor = Normalize(requested);
  std::vector<base::FilePath> missing;
  while (!base::PathExists(ancestor)) {
    const auto parent = ancestor.DirName();
    if (parent == ancestor) {
      return base::unexpected("ENOENT: storage path has no existing ancestor");
    }
    missing.push_back(ancestor.BaseName());
    ancestor = parent;
  }
  if (!base::DirectoryExists(ancestor)) {
    return base::unexpected(
        "ENOTDIR: application storage path is not a directory");
  }
  base::FilePath canonical;
  if (!base::NormalizeFilePath(ancestor, &canonical)) {
    return base::unexpected(
        "ERR_FS_OPERATION_FAILED: cannot resolve storage path");
  }
  for (auto item = missing.rbegin(); item != missing.rend(); ++item) {
    if (item->value() == FILE_PATH_LITERAL("..")) {
      canonical = canonical.DirName();
    } else if (item->value() != FILE_PATH_LITERAL(".")) {
      canonical = canonical.Append(*item);
    }
  }
  return Normalize(canonical);
}

struct ValidatedPaths {
  base::FilePath requested;
  base::FilePath host;
  base::FilePath initial_user_data;
  base::FilePath legacy_session_data;
};

base::expected<ValidatedPaths, std::string> ValidatePaths(
    const base::FilePath& requested,
    const base::FilePath& host,
    const base::FilePath& initial_user_data,
    const base::FilePath& legacy_session_data) {
  ValidatedPaths paths;
  const std::pair<const base::FilePath*, base::FilePath*> entries[] = {
      {&requested, &paths.requested},
      {&host, &paths.host},
      {&initial_user_data, &paths.initial_user_data},
      {&legacy_session_data, &paths.legacy_session_data}};
  for (const auto& [input, output] : entries) {
    auto canonical = CanonicalDirectory(*input);
    if (!canonical.has_value()) {
      return base::unexpected(canonical.error());
    }
    *output = std::move(*canonical);
  }
  return paths;
}

struct StorageLease {
  base::File lock;
  base::FilePath path;
};

base::expected<std::unique_ptr<StorageLease>, std::string> AcquireLease(
    const base::FilePath& path) {
  auto canonical = CanonicalDirectory(path);
  if (!canonical.has_value() || !SamePath(*canonical, path)) {
    return base::unexpected(canonical.has_value()
                                ? "ERR_RESOURCE_BUSY: storage path changed"
                                : canonical.error());
  }
  base::File::Error error;
  if (!base::CreateDirectoryAndGetError(path, &error)) {
    return base::unexpected(
        "ERR_FS_OPERATION_FAILED: cannot create storage directory: " +
        base::File::ErrorToString(error));
  }
  auto lease = std::make_unique<StorageLease>();
  lease->path = path;
  lease->lock.Initialize(path.AppendASCII(".xenon-storage.lock"),
                         base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_READ |
                             base::File::FLAG_WRITE);
  if (!lease->lock.IsValid()) {
    return base::unexpected(
        "ERR_FS_OPERATION_FAILED: cannot open storage lease");
  }
#if BUILDFLAG(IS_FUCHSIA)
  return base::unexpected("ERR_NOT_SUPPORTED: application storage locking");
#else
  if (lease->lock.Lock(base::File::LockMode::kExclusive) !=
      base::File::FILE_OK) {
    return base::unexpected(
        "ERR_RESOURCE_BUSY: storage directory is already in use");
  }
  return lease;
#endif
}

class ContextMarker final : public base::SupportsUserData::Data {
 public:
  ContextMarker(std::string container, base::OnceClosure destroyed)
      : container(std::move(container)), destroyed_(std::move(destroyed)) {}
  ~ContextMarker() override {
    // Profile clears user data after shutting down storage partitions. Do not
    // release its lease or permit a new owner from inside Profile destruction.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(destroyed_));
  }
  const std::string container;

 private:
  base::OnceClosure destroyed_;
};

class StorageOwner final
    : public base::RefCountedDeleteOnSequence<StorageOwner> {
 public:
  StorageOwner(content::BrowserContext* host,
               std::string container,
               std::string owner,
               base::FilePath user_data,
               ElectronAppStorageContextFactory create,
               ElectronAppStorageContextDestroyer destroy)
      : base::RefCountedDeleteOnSequence<StorageOwner>(
            base::SequencedTaskRunner::GetCurrentDefault()),
        host_(host->GetWeakPtr()),
        container_(std::move(container)),
        owner_(std::move(owner)),
        initial_user_data_(Normalize(user_data)),
        user_data_(initial_user_data_),
        legacy_session_data_(content::GetStoragePartitionPath(
            host,
            LegacyConfig(host, container_, ""))),
        create_(create ? std::move(create) : TestHooks().create),
        destroy_(destroy ? std::move(destroy) : TestHooks().destroy) {
    shutdown_ =
        ElectronSessionShutdownNotifierFactory::GetInstance()
            ->Get(host)
            ->Subscribe(base::BindOnce(&RetireElectronAppStorage, owner_));
  }

  bool active() const {
    return !retired_ && host_ && !host_->ShutdownStarted();
  }
  content::BrowserContext* Peek() const {
    return active() ? (context_ ? context_.get() : host_.get()) : nullptr;
  }
  const std::string& container() const { return container_; }
  content::BrowserContext* host() const { return host_.get(); }
  base::FilePath user_data() const {
    return active() ? user_data_ : base::FilePath();
  }

  void PrepareUserData(base::OnceCallback<void(std::string)> callback) {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](base::FilePath path) -> std::string {
              base::File::Error error;
              if (!base::CreateDirectoryAndGetError(path, &error)) {
                return "ERR_FS_OPERATION_FAILED: cannot create application "
                       "userData: " +
                       base::File::ErrorToString(error);
              }
              return {};
            },
            initial_user_data_),
        base::BindOnce(
            [](scoped_refptr<StorageOwner> self,
               base::OnceCallback<void(std::string)> callback,
               std::string error) {
              std::move(callback).Run(self->active() ? std::move(error)
                                                     : std::string(kRetired));
            },
            base::WrapRefCounted(this), std::move(callback)));
  }

  void Ensure(ElectronAppStorageReady callback) {
    if (!active()) {
      ReplyReady(std::move(callback), nullptr, kRetired);
      return;
    }
    if (updating_) {
      ReplyReady(std::move(callback), nullptr,
                 "ERR_RESOURCE_BUSY: a storage path update is pending");
      return;
    }
    if (releasing_) {
      ReplyReady(
          std::move(callback), nullptr,
          "ERR_RESOURCE_BUSY: application storage is finishing shutdown");
      return;
    }
    if (ready_) {
      ReplyReady(std::move(callback), Peek(), "");
      return;
    }
    waiters_.push_back(std::move(callback));
    if (preparing_) {
      return;
    }
    if (!Redirected()) {
      ready_ = true;
      FinishWaiters("");
      return;
    }
    preparing_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::BindOnce(&AcquireLease, SessionPath()),
        base::BindOnce(&StorageOwner::LeaseReady, base::WrapRefCounted(this)));
  }

  void Path(base::Value arguments, ElectronAppStoragePathReply callback) {
    const auto* dict = arguments.GetIfDict();
    const auto* method = dict ? dict->FindString("method") : nullptr;
    const auto* name = dict ? dict->FindString("name") : nullptr;
    if (!active()) {
      ReplyPath(std::move(callback), {}, kRetired);
      return;
    }
    if (!method || !name || (*name != "userData" && *name != "sessionData")) {
      ReplyPath(
          std::move(callback), {},
          "ERR_INVALID_ARG_VALUE: invalid application storage path request");
      return;
    }
    if (*method == "get") {
      // Permission check handlers can synchronously read app paths while
      // Browser waits for their result. Complete this authenticated, zero-IO
      // read on the current call stack; a posted reply would deadlock
      // SyncWatch.
      scoped_refptr keep_alive(this);
      std::move(callback).Run(
          base::Value((*name == "userData" ? user_data_ : SessionPath())
                          .AsUTF8Unsafe()),
          "");
      return;
    }
    const auto* text = dict->FindString("path");
    if (*method != "set" || !text || text->find('\0') != std::string::npos ||
        !base::FilePath::FromUTF8Unsafe(*text).IsAbsolute()) {
      ReplyPath(
          std::move(callback), {},
          "ERR_INVALID_ARG_VALUE: Path must be absolute and contain no NUL");
      return;
    }
    if (updating_ || releasing_) {
      ReplyPath(std::move(callback), {},
                "ERR_RESOURCE_BUSY: a storage path update is pending");
      return;
    }
    updating_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(&ValidatePaths, base::FilePath::FromUTF8Unsafe(*text),
                       host_->GetPath(), initial_user_data_,
                       legacy_session_data_),
        base::BindOnce(&StorageOwner::CommitPath, base::WrapRefCounted(this),
                       *name, std::move(callback)));
  }

  void Retire() {
    if (retired_) {
      return;
    }
    retired_ = true;
    shutdown_ = {};
    FinishWaiters(kRetired);
    if (context_) {
      DestroyContext(std::move(context_));
    } else if (!preparing_) {
      ReleaseLease();
    }
  }

 private:
  friend class base::RefCountedDeleteOnSequence<StorageOwner>;
  friend class base::DeleteHelper<StorageOwner>;
  ~StorageOwner() {
    DCHECK(!context_);
    // Normal teardown releases the lease on a worker after the storage and IO
    // barriers. Browser shutdown can instead discard that continuation while
    // destroying UI task queues, after ThreadPool shutdown has completed.
    // BrowserMainLoop already allows blocking during this teardown, so use
    // ordinary RAII for the remaining lease rather than posting new pool work.
    lease_.reset();
    ReleaseReservation();
  }

  base::FilePath SessionPath() const {
    return session_data_.value_or(
        (SamePath(user_data_, initial_user_data_) ||
         (!canonical_initial_user_data_.empty() &&
          SamePath(user_data_, canonical_initial_user_data_)))
            ? legacy_session_data_
            : user_data_);
  }
  bool Redirected() const {
    const auto path = SessionPath();
    return !SamePath(path, legacy_session_data_) &&
           (canonical_legacy_session_data_.empty() ||
            !SamePath(path, canonical_legacy_session_data_));
  }

  void ReplyReady(ElectronAppStorageReady callback,
                  content::BrowserContext* context,
                  std::string error) {
    auto weak_context = context ? context->GetWeakPtr()
                                : base::WeakPtr<content::BrowserContext>();
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](scoped_refptr<StorageOwner> self,
               ElectronAppStorageReady callback,
               base::WeakPtr<content::BrowserContext> context,
               std::string error) {
              if (!self->active() || (error.empty() && !context)) {
                context.reset();
                error = kRetired;
              }
              std::move(callback).Run(context.get(), std::move(error));
            },
            base::WrapRefCounted(this), std::move(callback), weak_context,
            std::move(error)));
  }
  void ReplyPath(ElectronAppStoragePathReply callback,
                 base::Value value,
                 std::string error) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](scoped_refptr<StorageOwner> self,
               ElectronAppStoragePathReply callback, base::Value value,
               std::string error) {
              if (!self->active()) {
                value = base::Value();
                error = kRetired;
              }
              std::move(callback).Run(std::move(value), std::move(error));
            },
            base::WrapRefCounted(this), std::move(callback), std::move(value),
            std::move(error)));
  }
  void CommitPath(std::string name,
                  ElectronAppStoragePathReply callback,
                  base::expected<ValidatedPaths, std::string> result) {
    updating_ = false;
    if (!active() || !result.has_value()) {
      ReplyPath(std::move(callback), {}, active() ? result.error() : kRetired);
      return;
    }
    auto user_data = name == "userData" ? result->requested : user_data_;
    auto explicit_session = name == "sessionData"
                                ? std::make_optional(result->requested)
                                : session_data_;
    auto effective = explicit_session.value_or(
        SamePath(user_data, initial_user_data_) ||
                SamePath(user_data, result->initial_user_data)
            ? legacy_session_data_
            : user_data);
    auto is_legacy = [&](const base::FilePath& path) {
      return SamePath(path, legacy_session_data_) ||
             SamePath(path, result->legacy_session_data);
    };
    if ((ready_ || preparing_) && !SamePath(effective, SessionPath()) &&
        !(is_legacy(effective) && is_legacy(SessionPath()))) {
      ReplyPath(
          std::move(callback), {},
          "ERR_INVALID_STATE: session storage has already been initialized");
      return;
    }
    if (!is_legacy(effective)) {
      // Never open a second Profile on the host Profile itself or beneath it.
      // The legacy session root is handled above without creating a Profile.
      if (OverlappingPaths(effective, result->host)) {
        ReplyPath(
            std::move(callback), {},
            "ERR_RESOURCE_BUSY: application storage overlaps the host Profile");
        return;
      }
      for (const auto& [path, owner] : Reservations()) {
        if (owner != owner_ && OverlappingPaths(path, effective)) {
          ReplyPath(std::move(callback), {},
                    "ERR_RESOURCE_BUSY: storage directory belongs to another "
                    "application");
          return;
        }
      }
      if (reserved_.empty() || !SamePath(reserved_, effective)) {
        ReleaseReservation();
        Reservations()[effective] = owner_;
        reserved_ = effective;
      }
    } else if (!ready_ && !preparing_) {
      ReleaseReservation();
    }
    canonical_initial_user_data_ = std::move(result->initial_user_data);
    canonical_legacy_session_data_ = std::move(result->legacy_session_data);
    user_data_ = std::move(user_data);
    session_data_ = std::move(explicit_session);
    ReplyPath(std::move(callback),
              base::Value(result->requested.AsUTF8Unsafe()), "");
  }
  void LeaseReady(
      base::expected<std::unique_ptr<StorageLease>, std::string> lease) {
    if (!lease.has_value()) {
      preparing_ = false;
      FinishWaiters(active() ? lease.error() : kRetired);
      if (!active()) {
        ReleaseLease();
      }
      return;
    }
    lease_ = std::move(*lease);
    if (!active()) {
      preparing_ = false;
      ReleaseLease();
      return;
    }
    auto callback = base::BindOnce(&StorageOwner::ContextCreated,
                                   base::WrapRefCounted(this));
    if (create_ && destroy_) {
      create_.Run(SessionPath(), std::move(callback));
    } else {
      std::move(callback).Run(
          nullptr,
          "ERR_NOT_SUPPORTED: application Profile factory is unavailable");
    }
  }
  void ContextCreated(std::unique_ptr<content::BrowserContext> context,
                      std::string error) {
    preparing_ = false;
    if (!context) {
      FinishWaiters(error.empty()
                        ? "ERR_FAILED: application Profile creation failed"
                        : error);
      ReleaseLease();
      return;
    }
    context->SetUserData(
        kContextMarker,
        std::make_unique<ContextMarker>(
            container_,
            content::MakeStorageServiceShutdownClosure(
                context.get(), base::BindOnce(&StorageOwner::ContextDestroyed,
                                              base::WrapRefCounted(this)))));
    if (!error.empty() || !active()) {
      FinishWaiters(active() ? error : kRetired);
      releasing_ = true;
      DestroyContext(std::move(context));
      return;
    }
    context_ = std::move(context);
    ready_ = true;
    FinishWaiters("");
  }
  void DestroyContext(std::unique_ptr<content::BrowserContext> context) {
    CHECK(destroy_);
    destroy_.Run(std::move(context));
  }
  void ContextDestroyed() {
    // StoragePartition shutdown posts work to IO. Release the directory lease
    // only after that sequence has passed the shutdown messages.
    if (!content::GetIOThreadTaskRunner({})->PostTaskAndReply(
            FROM_HERE, base::DoNothing(),
            base::BindOnce(&StorageOwner::ReleaseLease,
                           base::WrapRefCounted(this)))) {
      ReleaseLease();
    }
  }
  void ReleaseLease() {
    if (!lease_) {
      if (retired_) {
        ReleaseReservation();
      }
      return;
    }
    releasing_ = true;
    base::ThreadPool::PostTaskAndReply(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::BindOnce([](std::unique_ptr<StorageLease>) {}, std::move(lease_)),
        base::BindOnce(
            [](scoped_refptr<StorageOwner> self) {
              self->releasing_ = false;
              if (self->retired_) {
                self->ReleaseReservation();
              }
            },
            base::WrapRefCounted(this)));
  }
  void ReleaseReservation() {
    if (!reserved_.empty()) {
      auto found = Reservations().find(reserved_);
      if (found != Reservations().end() && found->second == owner_) {
        Reservations().erase(found);
      }
      reserved_.clear();
    }
  }
  void FinishWaiters(std::string error) {
    auto waiters = std::move(waiters_);
    for (auto& callback : waiters) {
      ReplyReady(std::move(callback), error.empty() ? Peek() : nullptr, error);
    }
  }

  const base::WeakPtr<content::BrowserContext> host_;
  const std::string container_;
  const std::string owner_;
  const base::FilePath initial_user_data_;
  base::FilePath canonical_initial_user_data_;
  base::FilePath user_data_;
  const base::FilePath legacy_session_data_;
  base::FilePath canonical_legacy_session_data_;
  std::optional<base::FilePath> session_data_;
  base::FilePath reserved_;
  const ElectronAppStorageContextFactory create_;
  const ElectronAppStorageContextDestroyer destroy_;
  bool updating_ = false;
  bool preparing_ = false;
  bool ready_ = false;
  bool releasing_ = false;
  bool retired_ = false;
  std::vector<ElectronAppStorageReady> waiters_;
  std::unique_ptr<StorageLease> lease_;
  std::unique_ptr<content::BrowserContext> context_;
  base::CallbackListSubscription shutdown_;
};

}  // namespace

base::expected<void, std::string> RegisterElectronAppStorage(
    content::BrowserContext* host,
    const std::string& container,
    const std::string& owner,
    const base::FilePath& initial_user_data,
    ElectronAppStorageContextFactory factory,
    ElectronAppStorageContextDestroyer destroyer) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!host || host->ShutdownStarted() || container.empty() || owner.empty() ||
      !initial_user_data.IsAbsolute()) {
    return base::unexpected(
        "ERR_INVALID_ARG_VALUE: invalid application storage owner");
  }
  if (Owners().contains(owner)) {
    return base::unexpected(
        "ERR_INVALID_STATE: application storage owner is already registered");
  }
  Owners()[owner] = base::MakeRefCounted<StorageOwner>(
      host, container, owner, initial_user_data, std::move(factory),
      std::move(destroyer));
  return {};
}

content::BrowserContext* PeekElectronAppStorageContext(
    const std::string& owner) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto found = Owners().find(owner);
  return found == Owners().end() ? nullptr : found->second->Peek();
}

base::FilePath GetElectronAppStorageUserDataPath(const std::string& owner) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto found = Owners().find(owner);
  return found == Owners().end() ? base::FilePath()
                                 : found->second->user_data();
}

void PrepareElectronAppStorageUserData(
    const std::string& owner,
    base::OnceCallback<void(std::string)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto found = Owners().find(owner);
  if (found != Owners().end()) {
    found->second->PrepareUserData(std::move(callback));
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::string(kRetired)));
}

void EnsureElectronAppStorageContext(const std::string& owner,
                                     ElectronAppStorageReady callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto found = Owners().find(owner);
  if (found != Owners().end()) {
    found->second->Ensure(std::move(callback));
  } else {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), nullptr, std::string(kRetired)));
  }
}

void CallElectronAppStoragePath(const std::string& owner,
                                base::Value arguments,
                                ElectronAppStoragePathReply callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto found = Owners().find(owner);
  if (found != Owners().end()) {
    found->second->Path(std::move(arguments), std::move(callback));
  } else {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), base::Value(),
                                  std::string(kRetired)));
  }
}

void RetireElectronAppStorage(const std::string& owner) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto retired = Owners().extract(owner);
  if (!retired.empty()) {
    retired.mapped()->Retire();
  }
}

std::optional<content::StoragePartitionConfig>
GetElectronAppStorageSessionConfig(content::BrowserContext* context,
                                   const std::string& container,
                                   const std::string& partition) {
  auto* marker =
      context
          ? static_cast<ContextMarker*>(context->GetUserData(kContextMarker))
          : nullptr;
  if (!marker || marker->container != container) {
    return std::nullopt;
  }
  if (partition.empty()) {
    return content::StoragePartitionConfig::CreateDefault(context);
  }
  // This BrowserContext already isolates the application. Repeating its
  // container hash in every named partition needlessly lengthens database
  // paths, which can prevent IndexedDB from opening them on Windows. Keep the
  // legacy container namespace only for applications sharing the host profile.
  return content::StoragePartitionConfig::Create(
      context, "electron",
      base::HexEncodeLower(crypto::hash::Sha256(partition)),
      !base::StartsWith(partition, "persist:"));
}

void SetElectronAppStorageContextFactoryForTesting(
    ElectronAppStorageContextFactory factory,
    ElectronAppStorageContextDestroyer destroyer) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  CHECK(Owners().empty());
  TestHooks() = {std::move(factory), std::move(destroyer)};
}

}  // namespace xenon::ipc

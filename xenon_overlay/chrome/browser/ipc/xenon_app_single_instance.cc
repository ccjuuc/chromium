// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_single_instance.h"

#include <deque>
#include <set>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_single_instance_transport.h"

namespace xenon::ipc {
namespace {

using Error = XenonAppSingleInstanceBroker::Error;
using Notification = XenonAppSingleInstanceBroker::Notification;

base::expected<std::string, Error> SerializeNotification(
    const Notification& notification,
    std::string_view lease,
    std::string_view request_id) {
  if (!notification.working_directory.IsAbsolute() ||
      !notification.additional_data.is_dict()) {
    return base::unexpected(Error{
        "ERR_INVALID_ARG_VALUE",
        "Single-instance arguments require an absolute cwd and JSON object"});
  }
  for (const auto& argument : notification.argv) {
    if (!argument.is_string() ||
        argument.GetString().find('\0') != std::string::npos) {
      return base::unexpected(
          Error{"ERR_INVALID_ARG_VALUE", "argv must contain NUL-free strings"});
    }
  }
  auto message =
      base::DictValue()
          .Set("version", 1)
          .Set("lease", lease)
          .Set("requestId", request_id)
          .Set("argv", notification.argv.Clone())
          .Set("cwd", notification.working_directory.AsUTF8Unsafe())
          .Set("additionalData", notification.additional_data.Clone());
  std::string json;
  if (!base::JSONWriter::Write(message, &json, 64) ||
      json.size() > XenonAppSingleInstanceTransport::kMaxMessageBytes) {
    return base::unexpected(
        Error{"ERR_OUT_OF_RANGE",
              "Single-instance JSON exceeds its size/depth limit"});
  }
  return json;
}

struct DecodedNotification {
  std::string request_id;
  Notification notification;
};

std::optional<DecodedNotification> DeserializeNotification(
    std::string_view message) {
  auto value = base::JSONReader::ReadDict(message, base::JSON_PARSE_RFC, 64);
  if (!value || !value->FindList("argv") || !value->FindString("cwd") ||
      !value->FindDict("additionalData") || !value->FindString("requestId")) {
    return std::nullopt;
  }
  return DecodedNotification{
      *value->FindString("requestId"),
      Notification{value->FindList("argv")->Clone(),
                   base::FilePath::FromUTF8Unsafe(*value->FindString("cwd")),
                   base::Value(value->FindDict("additionalData")->Clone())}};
}

std::string PathIdentity(const base::FilePath& path) {
  auto value =
      path.NormalizePathSeparators().StripTrailingSeparators().AsUTF8Unsafe();
#if BUILDFLAG(IS_WIN)
  value = base::ToLowerASCII(value);
#endif
  return value;
}

}  // namespace

struct XenonAppSingleInstanceBroker::Entry {
  Entry() = default;
  ~Entry() {
    // Stop notifications before removing rendezvous data, and release the OS
    // lock last. Never unlink the lock file: another process may have opened
    // that same inode while waiting to acquire it.
    transport.reset();
    if (!metadata_path.empty()) {
      base::DeleteFile(metadata_path);
    }
  }

  std::string owner;
  std::string lease;
  base::File lock;
  base::FilePath metadata_path;
  std::unique_ptr<XenonAppSingleInstanceTransport> transport;
  std::set<std::string> recent_requests;
  std::deque<std::string> request_order;
};

XenonAppSingleInstanceBroker::XenonAppSingleInstanceBroker(
    NotificationCallback callback)
    : notification_callback_(std::move(callback)) {}

XenonAppSingleInstanceBroker::~XenonAppSingleInstanceBroker() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  weak_factory_.InvalidateWeakPtrs();
  ReleaseAll();
}

base::expected<Notification, Error>
XenonAppSingleInstanceBroker::NotificationFromRequest(
    const std::vector<std::string>& launch_argv,
    const base::FilePath& executable,
    const base::FilePath& working_directory,
    const base::DictValue& request) {
  const auto* data = request.FindDict("additionalData");
  if (!data) {
    return base::unexpected(
        Error{"ERR_INVALID_ARG_VALUE", "additionalData must be a JSON object"});
  }
  base::ListValue argv;
  if (launch_argv.empty()) {
    argv.Append(executable.AsUTF8Unsafe());
  } else {
    for (const auto& argument : launch_argv) {
      argv.Append(argument);
    }
  }
  return Notification{std::move(argv), working_directory,
                      base::Value(data->Clone())};
}

base::expected<XenonAppSingleInstanceBroker::Notification,
               XenonAppSingleInstanceBroker::Error>
XenonAppSingleInstanceBroker::NotificationFromNativeRequest(
    const std::vector<std::string>& launch_argv,
    const base::FilePath& executable,
    const base::DictValue& request) {
  const auto* runtime = request.FindDict("__xenonRuntime");
  const auto* value =
      runtime ? runtime->FindString("workingDirectory") : nullptr;
  if (!value || value->find('\0') != std::string::npos) {
    return base::unexpected(
        Error{"ERR_INVALID_ARG_VALUE", "Missing native working directory"});
  }
  const auto directory = base::FilePath::FromUTF8Unsafe(*value);
  if (!directory.IsAbsolute()) {
    return base::unexpected(Error{"ERR_INVALID_ARG_VALUE",
                                  "Native working directory must be absolute"});
  }
  return NotificationFromRequest(launch_argv, executable, directory, request);
}

base::expected<bool, Error> XenonAppSingleInstanceBroker::Request(
    const std::string& owner,
    const base::FilePath& app_path,
    const base::FilePath& host_user_data,
    Notification notification) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (owner.empty() || !app_path.IsAbsolute() || !host_user_data.IsAbsolute()) {
    return base::unexpected(
        Error{"ERR_INVALID_ARG_VALUE",
              "A trusted owner and absolute paths are required"});
  }
  const auto request_id = base::UnguessableToken::Create().ToString();
  auto serialized =
      SerializeNotification(notification, "validation", request_id);
  if (!serialized.has_value()) {
    return base::unexpected(std::move(serialized.error()));
  }
  const auto canonical_app = base::MakeAbsoluteFilePath(app_path);
  const auto canonical_profile = base::MakeAbsoluteFilePath(host_user_data);
  if (canonical_app.empty() || canonical_profile.empty()) {
    return base::unexpected(
        Error{"ERR_PATH_UNAVAILABLE",
              "Unable to canonicalize application lock paths"});
  }
  auto user = XenonAppSingleInstanceTransport::UserIdentity();
  if (!user.has_value()) {
    return base::unexpected(
        Error{"ERR_SINGLE_INSTANCE_UNAVAILABLE", std::move(user.error())});
  }
  const std::string identity = *user + "\n" + PathIdentity(canonical_profile) +
                               "\n" + PathIdentity(canonical_app);
  const std::string key = base::HexEncode(crypto::hash::Sha256(identity));
  // A single broker is essential on POSIX: fcntl locks are per process, so
  // opening/closing another descriptor for an owned lock could unlock it.
  if (auto existing = entries_.find(key); existing != entries_.end()) {
    if (existing->second->owner == owner) {
      return true;
    }
    notification_callback_.Run(existing->second->owner,
                               std::move(notification));
    return false;
  }
  if (Has(owner)) {
    return base::unexpected(
        Error{"ERR_INVALID_STATE",
              "The application owner already holds another lock"});
  }
  const auto directory =
      canonical_profile.AppendASCII("XenonAppLocks").AppendASCII(key);
  if (!base::CreateDirectory(directory)) {
    return base::unexpected(
        Error{"ERR_FS_OPERATION_FAILED",
              "Unable to create the application lock directory"});
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(directory, 0700)) {
    return base::unexpected(
        Error{"ERR_FS_OPERATION_FAILED",
              "Unable to protect the application lock directory"});
  }
#endif
  const auto lock_path = directory.AppendASCII("lock");
  const auto metadata_path = directory.AppendASCII("endpoint.json");
  uint32_t flags = base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_READ |
                   base::File::FLAG_WRITE;
#if BUILDFLAG(IS_POSIX)
  flags |= base::File::FLAG_NO_FOLLOW;
#endif
  base::File lock(lock_path, flags);
  if (!lock.IsValid()) {
    return base::unexpected(Error{"ERR_FS_OPERATION_FAILED",
                                  "Unable to open the application lock"});
  }
  // Allow a concurrent winner to finish publishing its listener, or a crashed
  // owner to release its OS lock. Never remove another owner's lock or kill it.
  const auto deadline = base::TimeTicks::Now() + base::Seconds(2);
  for (;;) {
    if (lock.Lock(base::File::LockMode::kExclusive) == base::File::FILE_OK) {
      auto entry = std::make_unique<Entry>();
      entry->owner = owner;
      entry->lease = base::UnguessableToken::Create().ToString();
      entry->lock = std::move(lock);
      auto transport = XenonAppSingleInstanceTransport::Listen(
          entry->lease,
          base::BindPostTaskToCurrentDefault(base::BindRepeating(
              &XenonAppSingleInstanceBroker::Deliver,
              weak_factory_.GetWeakPtr(), key, owner, entry->lease)));
      if (!transport.has_value()) {
        return base::unexpected(Error{"ERR_SINGLE_INSTANCE_UNAVAILABLE",
                                      std::move(transport.error())});
      }
      entry->transport = std::move(*transport);
      auto metadata = base::DictValue()
                          .Set("version", 1)
                          .Set("lease", entry->lease)
                          .Set("endpoint", entry->transport->endpoint());
      auto json = base::WriteJson(metadata);
      if (!json || !base::ImportantFileWriter::WriteFileAtomically(
                       metadata_path, *json)) {
        return base::unexpected(
            Error{"ERR_FS_OPERATION_FAILED",
                  "Unable to publish the application listener"});
      }
      entry->metadata_path = metadata_path;
      entries_.emplace(key, std::move(entry));
      return true;
    }
    std::string metadata;
    if (base::ReadFileToStringWithMaxSize(metadata_path, &metadata, 16384)) {
      auto value =
          base::JSONReader::ReadDict(metadata, base::JSON_PARSE_RFC, 8);
      if (value && value->FindInt("version") == 1 &&
          value->FindString("endpoint") && value->FindString("lease")) {
        auto message = SerializeNotification(
            notification, *value->FindString("lease"), request_id);
        if (!message.has_value()) {
          return base::unexpected(std::move(message.error()));
        }
        auto notified = XenonAppSingleInstanceTransport::Notify(
            *value->FindString("endpoint"), *message);
        if (notified.has_value()) {
          return false;
        }
      }
    }
    if (base::TimeTicks::Now() >= deadline) {
      return base::unexpected(Error{
          "ERR_SINGLE_INSTANCE_UNAVAILABLE",
          "The primary application could not be notified within the timeout"});
    }
    base::PlatformThread::Sleep(base::Milliseconds(20));
  }
}

bool XenonAppSingleInstanceBroker::Has(const std::string& owner) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (const auto& [key, entry] : entries_) {
    if (entry->owner == owner) {
      return true;
    }
  }
  return false;
}

void XenonAppSingleInstanceBroker::Release(const std::string& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->second->owner == owner) {
      entries_.erase(it);
      return;
    }
  }
}

void XenonAppSingleInstanceBroker::ReleaseAll() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  entries_.clear();
}

void XenonAppSingleInstanceBroker::Deliver(const std::string& key,
                                           const std::string& owner,
                                           const std::string& lease,
                                           std::string message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto entry = entries_.find(key);
  if (entry == entries_.end() || entry->second->owner != owner ||
      entry->second->lease != lease) {
    return;
  }
  auto notification = DeserializeNotification(message);
  if (notification) {
    // A sender may retry when its ACK was lost after the event was queued.
    // Keep only a bounded recent window, scoped to this owner's lease.
    if (!entry->second->recent_requests.insert(notification->request_id)
             .second) {
      return;
    }
    entry->second->request_order.push_back(notification->request_id);
    if (entry->second->request_order.size() > 128) {
      entry->second->recent_requests.erase(
          entry->second->request_order.front());
      entry->second->request_order.pop_front();
    }
    notification_callback_.Run(owner, std::move(notification->notification));
  }
}

}  // namespace xenon::ipc

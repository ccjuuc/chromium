// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_crash_reporter_annotations.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "components/crash/core/common/crash_key.h"

namespace xenon::ipc {
namespace {

constexpr size_t kMaximumValueBytes = 20320;
constexpr size_t kMaximumExtraNames = 128;
constexpr std::string_view kExtraPrefix = "xenon-electron-extra-";

mojom::IpcResultPtr Success(base::Value value = base::Value()) {
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}

mojom::IpcResultPtr Failure(std::string error) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = std::move(error);
  return result;
}

bool ValidName(std::string_view name) {
  return !name.empty() && name.size() < 40 &&
         name.find('\0') == std::string_view::npos;
}

bool ValidValue(const base::Value& value) {
  return value.is_string() && value.GetString().find('\0') == std::string::npos;
}

struct AnnotationEntry {
  explicit AnnotationEntry(std::string annotation_name)
      : name(std::move(annotation_name)), annotation(name.c_str()) {}

  // Both the annotation and its name have stable storage for process lifetime.
  const std::string name;
  crash_reporter::CrashKeyString<kMaximumValueBytes> annotation;
  std::string value;
  uint64_t owner_id = 0;
};

struct AnnotationStore {
  base::Lock lock;
  std::map<std::string, std::unique_ptr<AnnotationEntry>> entries;
};

AnnotationStore& Store() {
  static base::NoDestructor<AnnotationStore> store;
  return *store;
}

std::string ExtraName(std::string_view name) {
  return std::string(kExtraPrefix) + std::string(name);
}

// Reserve all required slots before writing anything so failures are atomic.
mojom::IpcResultPtr Apply(uint64_t owner_id,
                          const std::map<std::string, std::string>& values) {
  auto& store = Store();
  base::AutoLock lock(store.lock);
  size_t names = 0;
  for (const auto& [name, entry] : store.entries) {
    if (name.starts_with(kExtraPrefix)) {
      ++names;
    }
  }
  for (const auto& [name, value] : values) {
    if (name.starts_with(kExtraPrefix) && !store.entries.contains(name)) {
      ++names;
    }
  }
  if (names > kMaximumExtraNames) {
    return Failure(
        "ERR_OUT_OF_RANGE: crashReporter supports 128 distinct "
        "extra parameter names per process lifetime");
  }
  for (const auto& [name, value] : values) {
    auto [it, inserted] = store.entries.try_emplace(name);
    if (inserted) {
      it->second = std::make_unique<AnnotationEntry>(name);
    }
    auto& entry = *it->second;
    // Preserve UTF-8 boundaries while respecting Electron's byte-size limit.
    base::TruncateUTF8ToByteSize(value, kMaximumValueBytes, &entry.value);
    entry.annotation.Set(entry.value);
    entry.owner_id = owner_id;
  }
  return Success();
}

}  // namespace

ElectronCrashReporterAnnotations::ElectronCrashReporterAnnotations(
    bool allow_start)
    : owner_id_([] {
        static std::atomic<uint64_t> next_id{1};
        return next_id.fetch_add(1, std::memory_order_relaxed);
      }()),
      allow_start_(allow_start) {}

ElectronCrashReporterAnnotations::~ElectronCrashReporterAnnotations() {
  Clear();
}

void ElectronCrashReporterAnnotations::Clear() {
  auto& store = Store();
  base::AutoLock lock(store.lock);
  for (auto& [name, entry] : store.entries) {
    if (entry->owner_id == owner_id_) {
      entry->annotation.Clear();
      entry->value.clear();
      entry->owner_id = 0;
    }
  }
  started_ = false;
}

mojom::IpcResultPtr ElectronCrashReporterAnnotations::Call(
    base::Value request) {
  if (!request.is_dict()) {
    return Failure(
        "ERR_INVALID_ARG_TYPE: crashReporter expects a request object");
  }
  const auto& dict = request.GetDict();
  const auto* operation = dict.FindString("operation");
  if (!operation) {
    return Failure(
        "ERR_INVALID_ARG_TYPE: crashReporter operation must be a string");
  }
  if (*operation == "start") {
    if (!allow_start_) {
      return Failure(
          "ERR_NOT_SUPPORTED: crashReporter.start is main-process only");
    }
    if (const auto* global = dict.Find("globalExtra")) {
      if (!global->is_dict()) {
        return Failure("ERR_INVALID_ARG_TYPE: globalExtra must be an object");
      }
      if (!global->GetDict().empty()) {
        return Failure(
            "ERR_NOT_SUPPORTED: crashReporter.globalExtra cannot "
            "be propagated to every application process");
      }
    }
    std::map<std::string, std::string> values;
    for (const auto* key : {"productName", "companyName", "appVersion"}) {
      if (const auto* value = dict.Find(key)) {
        if (!ValidValue(*value)) {
          return Failure(std::string("ERR_INVALID_ARG_TYPE: ") + key +
                         " must be a string without NUL characters");
        }
        values.emplace("xenon-electron-" + std::string(key),
                       value->GetString());
      }
    }
    if (const auto* extra = dict.Find("extra")) {
      if (!extra->is_dict()) {
        return Failure("ERR_INVALID_ARG_TYPE: extra must be an object");
      }
      for (const auto [name, value] : extra->GetDict()) {
        if (!ValidName(name) || !ValidValue(value)) {
          return Failure(
              "ERR_INVALID_ARG_VALUE: crashReporter extra keys must "
              "be 1-39 UTF-8 bytes and values strings without NUL");
        }
        values.emplace(ExtraName(name), value.GetString());
      }
    }
    if (started_) {
      return Success();
    }
    values.emplace("xenon-electron-role", "main");
    auto result = Apply(owner_id_, values);
    started_ = result->success;
    return result;
  }
  if (*operation == "addExtraParameter") {
    const auto* name = dict.FindString("key");
    const auto* value = dict.Find("value");
    if (!name || !value || !ValidName(*name) || !ValidValue(*value)) {
      return Failure(
          "ERR_INVALID_ARG_VALUE: crashReporter key must be 1-39 "
          "UTF-8 bytes and value a string without NUL");
    }
    return Apply(owner_id_, {{ExtraName(*name), value->GetString()}});
  }
  auto& store = Store();
  base::AutoLock lock(store.lock);
  if (*operation == "removeExtraParameter") {
    const auto* name = dict.FindString("key");
    if (!name || !ValidName(*name)) {
      return Failure(
          "ERR_INVALID_ARG_VALUE: crashReporter key must be 1-39 "
          "UTF-8 bytes without NUL");
    }
    const auto it = store.entries.find(ExtraName(*name));
    if (it != store.entries.end()) {
      it->second->annotation.Clear();
      it->second->value.clear();
      it->second->owner_id = 0;
    }
    return Success();
  }
  if (*operation == "getParameters") {
    base::DictValue values;
    for (const auto& [name, entry] : store.entries) {
      if (entry->owner_id && name.starts_with(kExtraPrefix)) {
        values.Set(name.substr(kExtraPrefix.size()), entry->value);
      }
    }
    return Success(base::Value(std::move(values)));
  }
  return Failure(
      "ERR_NOT_SUPPORTED: unsupported local crashReporter operation " +
      *operation);
}

}  // namespace xenon::ipc

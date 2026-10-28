// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_callbacks.h"

#include <algorithm>
#include <set>
#include <utility>

namespace xenon {
namespace {
constexpr char kWire[] = "__xenon_node_wire_type__";

const MacPlayerCallbackContract* FindContract(
    base::span<const MacPlayerCallbackContract> contracts,
    std::string_view method,
    std::string_view provider,
    std::optional<size_t> argument_count = {}) {
  for (const auto& contract : contracts) {
    if (contract.method == method && contract.provider == provider &&
        (!argument_count || contract.argument_count == *argument_count)) {
      return &contract;
    }
  }
  return nullptr;
}

bool IsWire(const base::Value& value, std::string_view type) {
  if (!value.is_dict()) {
    return false;
  }
  const auto* tag = value.GetDict().FindString(kWire);
  return tag && *tag == type;
}

bool ContainsNativeFunction(const base::Value& value, size_t depth = 0) {
  // The host bounds incoming value trees. Fail closed if used independently.
  if (depth > 48 || IsWire(value, "native_function")) {
    return true;
  }
  if (value.is_list()) {
    for (const auto& item : value.GetList()) {
      if (ContainsNativeFunction(item, depth + 1)) {
        return true;
      }
    }
  } else if (value.is_dict()) {
    for (const auto [key, item] : value.GetDict()) {
      if (ContainsNativeFunction(item, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

bool Fail(std::string* error, const char* message) {
  *error = message;
  return false;
}
}  // namespace

bool ReadMacPlayerCallbackRegistrations(
    base::span<const MacPlayerCallbackContract> contracts,
    std::string_view method,
    const base::ListValue& args,
    std::vector<MacPlayerCallbackRegistration>* registrations,
    std::string* error) {
  if (std::ranges::none_of(contracts, [method](const auto& contract) {
        return contract.method == method;
      })) {
    return Fail(error, "Unknown Mac player registration method");
  }
  if (args.size() != 1) {
    return Fail(error, "Mac player registration requires one argument");
  }
  std::vector<MacPlayerCallbackRegistration> parsed;
  std::set<int> callback_ids;
  auto add = [&](std::string_view provider, const base::Value& value) {
    if (!FindContract(contracts, method, provider) ||
        !IsWire(value, "callback")) {
      return Fail(error, "Unknown Mac player provider or invalid callback");
    }
    const int id = value.GetDict().FindInt("callback_id").value_or(0);
    if (id <= 0 || !callback_ids.insert(id).second) {
      return Fail(error, "Mac player provider callback ids must be unique");
    }
    parsed.push_back({id, std::string(method), std::string(provider)});
    return true;
  };
  if (FindContract(contracts, method, "")) {
    if (!add("", args[0])) {
      return false;
    }
  } else {
    if (!args[0].is_list() || args[0].GetList().size() > 128) {
      return Fail(error, "Invalid Mac player provider list");
    }
    std::set<std::string> provider_names;
    for (const auto& item : args[0].GetList()) {
      if (!item.is_dict() || item.GetDict().size() != 1) {
        return Fail(error, "Each Mac player provider must have one callback");
      }
      const auto [key, value] = *item.GetDict().begin();
      if (!provider_names.insert(std::string(key)).second) {
        return Fail(error, "Duplicate Mac player provider name");
      }
      if (!add(key, value)) {
        return false;
      }
    }
  }
  *registrations = std::move(parsed);
  return true;
}

bool ReadMacPlayerCompletion(
    base::span<const MacPlayerCallbackContract> contracts,
    const MacPlayerCallbackRegistration& registration,
    const base::ListValue& args,
    std::optional<MacPlayerCompletion>* completion,
    std::string* error) {
  const auto* contract = FindContract(contracts, registration.method,
                                      registration.provider, args.size());
  if (!contract) {
    return Fail(error, "Unknown Mac player callback contract or arity");
  }
  const bool notification = !contract->failure_arguments;
  if (!notification && args.empty()) {
    return Fail(error, "Mac player callback completion is missing");
  }
  const size_t plain_argument_count = args.size() - (notification ? 0 : 1);
  for (size_t i = 0; i < plain_argument_count; ++i) {
    if (ContainsNativeFunction(args[i])) {
      return Fail(error, "Unexpected Mac player native completion position");
    }
  }
  if (notification) {
    completion->reset();
    return true;
  }
  const auto& value = args.back();
  if (!IsWire(value, "native_function")) {
    return Fail(error, "Mac player callback completion is missing");
  }
  const auto& wire = value.GetDict();
  const int id = wire.FindInt("instance_id").value_or(0);
  const auto* token = wire.FindString("owner_token");
  if (id <= 0 || !token || token->empty()) {
    return Fail(error, "Mac player callback completion owner is missing");
  }
  *completion = MacPlayerCompletion{id, *token, contract->failure_arguments()};
  return true;
}

base::Value MakeMacPlayerReply(base::Value result,
                               const std::string& error,
                               base::ListValue accepted_callback_ids) {
  return base::Value(
      base::DictValue()
          .Set("protocolVersion", 1)
          .Set("result", std::move(result))
          .Set("error", error)
          .Set("acceptedCallbackIds", std::move(accepted_callback_ids)));
}

}  // namespace xenon

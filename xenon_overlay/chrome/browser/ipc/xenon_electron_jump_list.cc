// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_jump_list.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <shobjidl.h>
#include <windows.h>

#include <propkey.h>
#include <shlobj.h>
#include <wrl/client.h>

#include "base/win/win_util.h"
#endif

namespace xenon::ipc {
namespace {
bool ValidText(const base::Value* value, bool nonempty = false) {
  return value && value->is_string() &&
         (!nonempty || !value->GetString().empty()) &&
         value->GetString().size() <= 32767 &&
         value->GetString().find('\0') == std::string::npos;
}

std::string Type(const base::DictValue& item, const char* fallback) {
  const auto* type = item.FindString("type");
  return type ? *type : fallback;
}

#if BUILDFLAG(IS_WIN)
using Microsoft::WRL::ComPtr;

HRESULT MakeItem(const base::DictValue& item, ComPtr<IUnknown>* output) {
  const auto type = Type(item, "task");
  if (type == "file") {
    const auto path = base::UTF8ToWide(*item.FindString("path"));
    ComPtr<IShellItem> shell_item;
    HRESULT status = SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                 IID_PPV_ARGS(&shell_item));
    if (FAILED(status)) {
      return status;
    }
    return shell_item.As(output);
  }
  ComPtr<IShellLinkW> link;
  HRESULT status = CoCreateInstance(CLSID_ShellLink, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
  if (FAILED(status)) {
    return status;
  }
  ComPtr<IPropertyStore> properties;
  if (FAILED(status = link.As(&properties))) {
    return status;
  }
  if (type == "separator") {
    PROPVARIANT separator = {};
    separator.vt = VT_BOOL;
    separator.boolVal = VARIANT_TRUE;
    if (FAILED(status = properties->SetValue(
                   PKEY_AppUserModel_IsDestListSeparator, separator))) {
      return status;
    }
  } else {
    if (FAILED(status = link->SetPath(
                   base::UTF8ToWide(*item.FindString("program")).c_str()))) {
      return status;
    }
    if (!base::win::SetStringValueForPropertyStore(
            properties.Get(), PKEY_Title,
            base::UTF8ToWide(*item.FindString("title")))) {
      return E_FAIL;
    }
    if (const auto* args = item.FindString("args")) {
      if (FAILED(status =
                     link->SetArguments(base::UTF8ToWide(*args).c_str()))) {
        return status;
      }
    }
    if (const auto* description = item.FindString("description")) {
      if (FAILED(status = link->SetDescription(
                     base::UTF8ToWide(*description).c_str()))) {
        return status;
      }
    }
    if (const auto* directory = item.FindString("workingDirectory")) {
      if (FAILED(status = link->SetWorkingDirectory(
                     base::UTF8ToWide(*directory).c_str()))) {
        return status;
      }
    }
    if (const auto* icon = item.FindString("iconPath")) {
      if (FAILED(status = link->SetIconLocation(
                     base::UTF8ToWide(*icon).c_str(),
                     item.FindInt("iconIndex").value_or(0)))) {
        return status;
      }
    }
  }
  if (FAILED(status = properties->Commit())) {
    return status;
  }
  return link.As(output);
}

std::string SetOnSta(std::string app_id, base::Value categories) {
  ComPtr<ICustomDestinationList> list;
  if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))) {
    return "error";
  }
  const auto id = base::UTF8ToWide(app_id);
  if (categories.is_none()) {
    return SUCCEEDED(list->DeleteList(id.c_str())) ? "ok" : "error";
  }
  if (FAILED(list->SetAppID(id.c_str()))) {
    return "error";
  }
  UINT slots = 0;
  ComPtr<IObjectArray> removed;
  if (FAILED(list->BeginList(&slots, IID_PPV_ARGS(&removed)))) {
    return "error";
  }
  std::string result = "ok";
  for (const auto& value : categories.GetList()) {
    const auto& category = value.GetDict();
    const auto type =
        Type(category, category.contains("name") ? "custom" : "tasks");
    HRESULT status = S_OK;
    if (type == "recent" || type == "frequent") {
      status = list->AppendKnownCategory(type == "recent" ? KDC_RECENT
                                                          : KDC_FREQUENT);
    } else {
      ComPtr<IObjectCollection> items;
      status = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&items));
      const auto* values = category.FindList("items");
      if (SUCCEEDED(status) && values) {
        for (const auto& entry : *values) {
          ComPtr<IUnknown> item;
          status = MakeItem(entry.GetDict(), &item);
          if (FAILED(status)) {
            break;
          }
          status = items->AddObject(item.Get());
          if (FAILED(status)) {
            break;
          }
        }
      }
      if (SUCCEEDED(status) && values && !values->empty()) {
        ComPtr<IObjectArray> array;
        status = items.As(&array);
        if (SUCCEEDED(status)) {
          status =
              type == "tasks"
                  ? list->AddUserTasks(array.Get())
                  : list->AppendCategory(
                        base::UTF8ToWide(*category.FindString("name")).c_str(),
                        array.Get());
        }
      }
    }
    if (FAILED(status)) {
      result = type == "custom" && status == E_ACCESSDENIED
                   ? "customCategoryAccessDenied"
               : status == static_cast<HRESULT>(0x80040F03L)
                   ? "fileTypeRegistrationError"
                   : "error";
      break;
    }
  }
  if (result == "ok" && SUCCEEDED(list->CommitList())) {
    return "ok";
  }
  list->AbortList();
  return result == "ok" ? "error" : result;
}
#endif
}  // namespace

std::string ValidateElectronJumpList(const base::Value& categories) {
  if (categories.is_none()) {
    return {};
  }
  if (!categories.is_list() || categories.GetList().size() > 128) {
    return "ERR_INVALID_ARG_TYPE: categories must be an array or null";
  }
  size_t count = 0;
  for (const auto& value : categories.GetList()) {
    if (!value.is_dict()) {
      return "ERR_INVALID_ARG_TYPE: category must be an object";
    }
    const auto& category = value.GetDict();
    if (category.contains("type") && !ValidText(category.Find("type"), true)) {
      return "ERR_INVALID_ARG_TYPE: category type must be a string";
    }
    const auto type =
        Type(category, category.contains("name") ? "custom" : "tasks");
    if (type != "recent" && type != "frequent" && type != "tasks" &&
        type != "custom") {
      return "ERR_INVALID_ARG_VALUE: invalid category type";
    }
    if (type == "custom" && !ValidText(category.Find("name"), true)) {
      return "ERR_INVALID_ARG_VALUE: custom category requires a name";
    }
    const auto* items = category.Find("items");
    if (!items) {
      continue;
    }
    if (!items->is_list()) {
      return "ERR_INVALID_ARG_TYPE: items must be an array";
    }
    count += items->GetList().size();
    if (count > 1024) {
      return "ERR_OUT_OF_RANGE: too many JumpList items";
    }
    for (const auto& item_value : items->GetList()) {
      if (!item_value.is_dict()) {
        return "ERR_INVALID_ARG_TYPE: item must be an object";
      }
      const auto& item = item_value.GetDict();
      if (item.contains("type") && !ValidText(item.Find("type"), true)) {
        return "ERR_INVALID_ARG_TYPE: item type must be a string";
      }
      const auto item_type = Type(item, "task");
      if (item_type != "task" && item_type != "file" &&
          item_type != "separator") {
        return "ERR_INVALID_ARG_VALUE: invalid item type";
      }
      if (item_type == "task" && (!ValidText(item.Find("program"), true) ||
                                  !ValidText(item.Find("title"), true))) {
        return "ERR_INVALID_ARG_VALUE: task requires program and title";
      }
      if (item_type == "file" && !ValidText(item.Find("path"), true)) {
        return "ERR_INVALID_ARG_VALUE: file requires a path";
      }
      for (const char* key :
           {"args", "description", "workingDirectory", "iconPath"}) {
        if (item.contains(key) && !ValidText(item.Find(key))) {
          return std::string("ERR_INVALID_ARG_VALUE: invalid ") + key;
        }
      }
      if (item.contains("iconIndex") && !item.FindInt("iconIndex")) {
        return "ERR_INVALID_ARG_TYPE: iconIndex must be an integer";
      }
    }
  }
  return {};
}

void SetElectronJumpList(
    std::string app_id,
    base::Value categories,
    base::OnceCallback<void(base::Value, const std::string&)> callback) {
  if (auto error = ValidateElectronJumpList(categories); !error.empty()) {
    std::move(callback).Run(base::Value(), std::move(error));
    return;
  }
#if BUILDFLAG(IS_WIN)
  // A separator outside Tasks is a documented OS failure, not a JS exception.
  if (categories.is_list()) {
    for (const auto& value : categories.GetList()) {
      const auto& category = value.GetDict();
      if (Type(category, category.contains("name") ? "custom" : "tasks") ==
          "custom") {
        if (const auto* items = category.FindList("items")) {
          for (const auto& item : *items) {
            if (Type(item.GetDict(), "task") == "separator") {
              std::move(callback).Run(
                  base::Value("customCategorySeparatorError"), "");
              return;
            }
          }
        }
      }
    }
  }
  base::ThreadPool::CreateCOMSTATaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE})
      ->PostTaskAndReplyWithResult(
          FROM_HERE,
          base::BindOnce(&SetOnSta, std::move(app_id), std::move(categories)),
          base::BindOnce(
              [](base::OnceCallback<void(base::Value, const std::string&)>
                     reply,
                 std::string status) {
                std::move(reply).Run(base::Value(std::move(status)), "");
              },
              std::move(callback)));
#else
  std::move(callback).Run(base::Value(),
                          "ERR_NOT_SUPPORTED: JumpLists require Windows");
#endif
}
}  // namespace xenon::ipc

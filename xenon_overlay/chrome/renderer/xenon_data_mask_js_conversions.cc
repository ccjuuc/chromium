// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/xenon_data_mask_js_conversions.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include "base/values.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace xenon {

namespace {

const base::Value* FindInDict(const base::DictValue& dict,
                              std::string_view primary,
                              std::string_view alternate = {}) {
  if (auto* v = dict.Find(primary)) {
    return v;
  }
  if (!alternate.empty()) {
    return dict.Find(alternate);
  }
  return nullptr;
}

bool GetBool(const base::DictValue& dict,
             std::string_view camel,
             std::string_view snake,
             bool default_value) {
  const base::Value* v = FindInDict(dict, camel, snake);
  if (!v) {
    return default_value;
  }
  std::optional<bool> b = v->GetIfBool();
  return b.value_or(default_value);
}

uint32_t GetUint32(const base::DictValue& dict,
                   std::string_view camel,
                   std::string_view snake,
                   uint32_t default_value) {
  const base::Value* v = FindInDict(dict, camel, snake);
  if (!v) {
    return default_value;
  }
  if (auto d = v->GetIfDouble()) {
    if (*d >= 0.0 && *d <= static_cast<double>(UINT32_MAX)) {
      return static_cast<uint32_t>(*d);
    }
  }
  if (auto i = v->GetIfInt()) {
    if (*i >= 0) {
      return static_cast<uint32_t>(*i);
    }
  }
  return default_value;
}

blink::String GetWtfString(const base::DictValue& dict,
                           std::string_view camel,
                           std::string_view snake,
                           const blink::String& default_value) {
  const base::Value* v = FindInDict(dict, camel, snake);
  if (!v) {
    return default_value;
  }
  if (const std::string* s = v->GetIfString()) {
    return blink::String::FromUtf8(*s);
  }
  return default_value;
}

void AppendWtfStringVector(const base::DictValue& dict,
                           std::string_view camel,
                           std::string_view snake,
                           blink::Vector<blink::String>* out) {
  const base::Value* v = FindInDict(dict, camel, snake);
  const base::ListValue* list = v ? v->GetIfList() : nullptr;
  if (!list) {
    return;
  }
  for (const base::Value& el : *list) {
    if (const std::string* s = el.GetIfString()) {
      out->push_back(blink::String::FromUtf8(*s));
    }
  }
}

void FillRegsFromDict(const base::DictValue& d,
                      blink::Vector<blink::String>* regs) {
  regs->clear();
  AppendWtfStringVector(d, "regs", "", regs);
  if (regs->empty()) {
    AppendWtfStringVector(d, "includeRegexes", "include_regexes", regs);
  }
}

void FillRegsXorFromDict(const base::DictValue& d,
                         blink::Vector<blink::String>* regs_xor) {
  regs_xor->clear();
  AppendWtfStringVector(d, "regsXor", "regs_xor", regs_xor);
  if (regs_xor->empty()) {
    AppendWtfStringVector(d, "excludeRegexes", "exclude_regexes", regs_xor);
  }
}

bool RegsIsAndFromDict(const base::DictValue& d) {
  if (FindInDict(d, "regsIsAnd", "regs_is_and")) {
    return GetBool(d, "regsIsAnd", "regs_is_and", false);
  }
  return GetBool(d, "includeRegexesRequireAll", "include_regexes_require_all",
                 false);
}

bool RegsXorIsAndFromDict(const base::DictValue& d) {
  if (FindInDict(d, "regsXorIsAnd", "regs_xor_is_and")) {
    return GetBool(d, "regsXorIsAnd", "regs_xor_is_and", false);
  }
  return GetBool(d, "excludeRegexesRequireAll", "exclude_regexes_require_all",
                 false);
}

void FillDictsFromDict(const base::DictValue& d,
                       blink::Vector<blink::String>* dicts) {
  dicts->clear();
  AppendWtfStringVector(d, "dicts", "", dicts);
  if (dicts->empty()) {
    AppendWtfStringVector(d, "dictionaryTerms", "dictionary_terms", dicts);
  }
}

bool DictsFuzzyFromDict(const base::DictValue& d) {
  if (FindInDict(d, "dictsFuzzyCompare", "dicts_fuzzy_compare")) {
    return GetBool(d, "dictsFuzzyCompare", "dicts_fuzzy_compare", false);
  }
  return GetBool(d, "dictionaryFuzzyCompare", "dictionary_fuzzy_compare", false);
}

blink::mojom::blink::MaskType ParseMaskType(const base::Value* v) {
  if (!v) {
    return blink::mojom::blink::MaskType::kReplace;
  }
  if (auto i = v->GetIfInt()) {
    return *i == static_cast<int>(blink::mojom::blink::MaskType::kBlur)
               ? blink::mojom::blink::MaskType::kBlur
               : blink::mojom::blink::MaskType::kReplace;
  }
  if (const std::string* s = v->GetIfString()) {
    if (*s == "blur" || *s == "kBlur") {
      return blink::mojom::blink::MaskType::kBlur;
    }
  }
  return blink::mojom::blink::MaskType::kReplace;
}

blink::mojom::blink::MaskItemPtr MaskItemFromDict(const base::DictValue& d) {
  auto item = blink::mojom::blink::MaskItem::New();
  item->tag_id = GetUint32(d, "tagId", "tag_id", 0);
  item->policy_name =
      GetWtfString(d, "policyName", "policy_name", blink::String());
  item->regs_is_and = RegsIsAndFromDict(d);
  FillRegsFromDict(d, &item->regs);
  FillRegsXorFromDict(d, &item->regs_xor);
  item->regs_xor_is_and = RegsXorIsAndFromDict(d);
  FillDictsFromDict(d, &item->dicts);
  item->dicts_fuzzy_compare = DictsFuzzyFromDict(d);
  item->mask_type = ParseMaskType(FindInDict(d, "maskType", "mask_type"));
  item->replace_begin = GetUint32(d, "replaceBegin", "replace_begin", 0);
  item->replace_end = GetUint32(d, "replaceEnd", "replace_end", 0);
  item->replace_text =
      GetWtfString(d, "replaceText", "replace_text", blink::String());
  item->recover = GetBool(d, "recover", "recover", false);
  item->is_content_mask = GetBool(d, "isContentMask", "is_content_mask", false);
  item->is_column_name_mask =
      GetBool(d, "isColumnNameMask", "is_column_name_mask", false);
  item->is_column_value_mask =
      GetBool(d, "isColumnValueMask", "is_column_value_mask", false);
  return item;
}

}  // namespace

bool BuildDataMaskRulesFromValue(const base::Value& value,
                                 blink::mojom::blink::DataMaskRulesPtr* out_rules) {
  if (!out_rules) {
    return false;
  }
  *out_rules = nullptr;

  const base::ListValue* items = nullptr;
  if (const base::DictValue* root = value.GetIfDict()) {
    const base::Value* raw_items = FindInDict(*root, "maskItems", "mask_items");
    items = raw_items ? raw_items->GetIfList() : nullptr;
  } else {
    items = value.GetIfList();
  }
  if (!items) {
    return false;
  }

  auto rules = blink::mojom::blink::DataMaskRules::New();
  for (const base::Value& entry : *items) {
    const base::DictValue* d = entry.GetIfDict();
    if (!d) {
      continue;
    }
    rules->mask_items.push_back(MaskItemFromDict(*d));
  }

  *out_rules = std::move(rules);
  return true;
}

bool BuildXPathConfigFromValue(const base::Value& value,
                               blink::mojom::blink::XPathConfigPtr* out_config) {
  if (!out_config) {
    return false;
  }
  *out_config = nullptr;
  const base::DictValue* d = value.GetIfDict();
  if (!d) {
    return false;
  }
  auto config = blink::mojom::blink::XPathConfig::New();
  AppendWtfStringVector(*d, "disableList", "disable_list", &config->disable_list);
  AppendWtfStringVector(*d, "whiteList", "white_list", &config->white_list);
  AppendWtfStringVector(*d, "downloadList", "download_list",
                        &config->download_list);
  *out_config = std::move(config);
  return true;
}

}  // namespace xenon

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/blink/renderer/core/frame/data_mask_applier.h"

#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/node_traversal.h"
#include "third_party/blink/renderer/core/dom/text.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/re2/src/re2/re2.h"

namespace blink {

namespace {

bool IsUnderNonMaskableElement(const Element* start) {
  for (const Element* e = start; e; e = e->parentElement()) {
    if (e->HasTagName(html_names::kScriptTag) ||
        e->HasTagName(html_names::kStyleTag)) {
      return true;
    }
  }
  return false;
}

bool RegexpMatchesUtf8(const std::string& utf8, const String& pattern) {
  RE2::Options options;
  options.set_log_errors(false);
  RE2 re(pattern.Utf8(), options);
  return re.ok() && RE2::PartialMatch(utf8, re);
}

bool TryApplyMaskItemToText(Text& text_node,
                            const mojom::blink::MaskItem& item) {
  if (!item.is_content_mask) {
    return false;
  }

  String original = text_node.data();
  if (original.empty()) {
    return false;
  }

  Element* parent = text_node.parentElement();
  if (!parent || IsUnderNonMaskableElement(parent)) {
    return false;
  }

  std::string utf8 = original.Utf8();

  for (const auto& xor_pat : item.regs_xor) {
    if (RegexpMatchesUtf8(utf8, xor_pat)) {
      return false;
    }
  }

  if (!item.regs.empty()) {
    if (item.regs_is_and) {
      for (const auto& pat : item.regs) {
        if (!RegexpMatchesUtf8(utf8, pat)) {
          return false;
        }
      }
    } else {
      bool any = false;
      for (const auto& pat : item.regs) {
        if (RegexpMatchesUtf8(utf8, pat)) {
          any = true;
          break;
        }
      }
      if (!any) {
        return false;
      }
    }
  }

  if (!item.dicts.empty()) {
    bool dict_hit = false;
    for (const auto& d : item.dicts) {
      if (original.contains(d)) {
        dict_hit = true;
        break;
      }
    }
    if (!dict_hit) {
      return false;
    }
  }

  if (item.mask_type == mojom::blink::MaskType::kBlur) {
    const AtomicString existing = parent->FastGetAttribute(html_names::kStyleAttr);
    static constexpr char kBlur[] = "filter: blur(4px);";
    if (existing.IsNull()) {
      parent->setAttribute(html_names::kStyleAttr, AtomicString(kBlur));
    } else if (!existing.GetString().contains("filter:")) {
      parent->setAttribute(
          html_names::kStyleAttr,
          AtomicString(existing.GetString() + " " + kBlur));
    }
    return true;
  }

  String new_text = original;
  std::string mutable_utf8 = new_text.Utf8();
  for (const auto& pat : item.regs) {
    RE2::Options options;
    options.set_log_errors(false);
    RE2 re(pat.Utf8(), options);
    if (re.ok()) {
      RE2::GlobalReplace(&mutable_utf8, re, item.replace_text.Utf8());
    }
  }
  new_text = String::FromUtf8(mutable_utf8);

  for (const auto& d : item.dicts) {
    new_text.Replace(d, item.replace_text);
  }

  if (new_text != original) {
    text_node.setData(new_text);
    return true;
  }
  return false;
}

}  // namespace

void ApplyDataMaskForLocalFrame(LocalFrame& frame) {
  Document* doc = frame.GetDocument();
  const mojom::blink::DataMaskRules* rules = frame.GetDataMaskRules();
  const bool active = rules && !rules->mask_items.empty();
  if (!active || !doc) {
    return;
  }

  for (Node& node : NodeTraversal::InclusiveDescendantsOf(*doc)) {
    auto* text = DynamicTo<Text>(node);
    if (!text) {
      continue;
    }
    for (const auto& item : rules->mask_items) {
      if (item && TryApplyMaskItemToText(*text, *item)) {
        break;
      }
    }
  }

  frame.EnsureDataMaskSubtreeObserver();
  frame.ScheduleDataMaskApplyPumpIfNeeded();
}

void ResetDataMaskPresentationState(LocalFrame& frame) {
  frame.ClearDataMaskMutationObserver();
  frame.data_mask_load_pump_scheduled_ = false;
}

}  // namespace blink

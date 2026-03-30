// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_RENDERER_XENON_DATA_MASK_JS_CONVERSIONS_H_
#define XENON_OVERLAY_CHROME_RENDERER_XENON_DATA_MASK_JS_CONVERSIONS_H_

#include "base/values.h"
#include "third_party/blink/public/mojom/frame/data_mask.mojom-blink.h"

namespace xenon {

bool BuildDataMaskRulesFromValue(const base::Value& value,
                                 blink::mojom::blink::DataMaskRulesPtr* out_rules);
bool BuildXPathConfigFromValue(const base::Value& value,
                               blink::mojom::blink::XPathConfigPtr* out_config);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_RENDERER_XENON_DATA_MASK_JS_CONVERSIONS_H_

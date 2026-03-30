// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_DATA_MASK_TO_MAIN_HOST_IMPL_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_DATA_MASK_TO_MAIN_HOST_IMPL_H_

#include <cstdint>
#include <string>

#include "content/public/browser/document_service.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/frame/data_mask.mojom.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace xenon {

// Browser receives `DataMaskToMain` from the renderer (e.g. `window.xenon`).
class XenonDataMaskToMainHostImpl
    : public content::DocumentService<blink::mojom::DataMaskToMain> {
 public:
  static void Create(content::RenderFrameHost* render_frame_host,
                     mojo::PendingReceiver<blink::mojom::DataMaskToMain> receiver);

  XenonDataMaskToMainHostImpl(const XenonDataMaskToMainHostImpl&) = delete;
  XenonDataMaskToMainHostImpl& operator=(const XenonDataMaskToMainHostImpl&) =
      delete;
  ~XenonDataMaskToMainHostImpl() override;

 private:
  XenonDataMaskToMainHostImpl(
      content::RenderFrameHost* render_frame_host,
      mojo::PendingReceiver<blink::mojom::DataMaskToMain> receiver);

  void SendDataToMain(int32_t request_id, const std::string& data) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_DATA_MASK_TO_MAIN_HOST_IMPL_H_

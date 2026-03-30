// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_RENDERER_JS_XENON_API_H_
#define XENON_OVERLAY_CHROME_RENDERER_JS_XENON_API_H_

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "gin/arguments.h"
#include "gin/weak_cell.h"
#include "gin/wrappable.h"
#include "v8/include/cppgc/visitor.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/frame/data_mask.mojom.h"
#include "v8/include/v8-forward.h"
#include "xenon_overlay/public/mojom/xenon_page_api.mojom.h"

namespace xenon {

// JavaScript `window.xenon` API: gin::Wrappable + document-scoped Mojo to
// xenon::mojom::XenonPageHost (mirrors Brave wallet's JS provider pattern).
class JSXenonApi final : public gin::Wrappable<JSXenonApi>,
                         public content::RenderFrameObserver {
 public:
  static constexpr gin::WrapperInfo kWrapperInfo = {
      {gin::kEmbedderNativeGin},
      gin::kXenonPageApi,
  };

  static void Install(content::RenderFrame* render_frame);

  explicit JSXenonApi(content::RenderFrame* render_frame);
  ~JSXenonApi() override;

  JSXenonApi(const JSXenonApi&) = delete;
  JSXenonApi& operator=(const JSXenonApi&) = delete;

  const gin::WrapperInfo* wrapper_info() const override;

  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override;

  void Trace(cppgc::Visitor* visitor) const final;

 private:
  void Ping(gin::Arguments* args);
  void GetApiVersion(gin::Arguments* args);
  void EchoObject(gin::Arguments* args);
  void WrapObjectWithBrowserMeta(gin::Arguments* args);
  void SendDataMaskRules(gin::Arguments* args);
  void SendDataMaskXPath(gin::Arguments* args);
  void SendDataMaskToMain(gin::Arguments* args);

  void WillReleaseScriptContext(v8::Local<v8::Context> context,
                                int32_t world_id) override;

  void OnDestruct() override;

  bool EnsureConnected();
  bool EnsureBlinkDataMaskToMainConnected();

  void OnPing(v8::Global<v8::Context> global_context,
              v8::Global<v8::Promise::Resolver> resolver_global,
              v8::Isolate* isolate,
              bool ok,
              const std::string& message);

  void OnGetApiVersion(v8::Global<v8::Context> global_context,
                       v8::Global<v8::Promise::Resolver> resolver_global,
                       v8::Isolate* isolate,
                       const std::string& version);

  void OnEchoObject(v8::Global<v8::Context> global_context,
                    v8::Global<v8::Promise::Resolver> resolver_global,
                    v8::Isolate* isolate,
                    base::Value result);

  void OnWrapObjectWithBrowserMeta(v8::Global<v8::Context> global_context,
                                   v8::Global<v8::Promise::Resolver> resolver_global,
                                   v8::Isolate* isolate,
                                   base::Value result);

  mojo::Remote<xenon::mojom::XenonPageHost> xenon_host_;
  mojo::Remote<blink::mojom::DataMaskToMain> blink_data_mask_to_main_;

  gin::WeakCellFactory<JSXenonApi> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_RENDERER_JS_XENON_API_H_

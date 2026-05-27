// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_BEIJING_JS_BEIJING_API_H_
#define CHROME_RENDERER_BEIJING_JS_BEIJING_API_H_

#include <map>
#include <memory>
#include <string>

#include <vector>

#include "base/sequence_checker.h"
#include "base/memory/raw_ptr.h"
#include "chrome/common/beijing/beijing_page_api.mojom.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "gin/arguments.h"
#include "gin/weak_cell.h"
#include "gin/wrappable.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-forward.h"

namespace content {
class V8ValueConverter;
}

namespace beijing {

// 供页面 window.beijing 调用的通用 V8 宿主对象实现 (封装 Mojo 并返回 Promise)
class JSBeijingApi final : public gin::Wrappable<JSBeijingApi>,
                           public content::RenderFrameObserver {
 public:
  static constexpr gin::WrapperInfo kWrapperInfo = {
      {gin::kEmbedderNativeGin},
      gin::kBeijingPageApi,
  };

  static void Install(content::RenderFrame* render_frame);

  explicit JSBeijingApi(content::RenderFrame* render_frame);
  ~JSBeijingApi() override;

  JSBeijingApi(const JSBeijingApi&) = delete;
  JSBeijingApi& operator=(const JSBeijingApi&) = delete;

  const gin::WrapperInfo* wrapper_info() const override;

  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override;

  void Trace(cppgc::Visitor* visitor) const final;

  private:
  void Ping(gin::Arguments* args);
  void GetApiVersion(gin::Arguments* args);
  void EchoObject(gin::Arguments* args);
  void WrapObjectWithBrowserMeta(gin::Arguments* args);
  void Subscribe(gin::Arguments* args);
  void Unsubscribe(gin::Arguments* args);
  void Publish(gin::Arguments* args);

  void WillReleaseScriptContext(v8::Local<v8::Context> context,
                                int32_t world_id) override;

  void OnDestruct() override;

  bool EnsureConnected();



  class BeijingSubscription;
  std::map<std::string, std::vector<cppgc::Member<BeijingSubscription>>> subscriptions_;

  mojo::Remote<beijing::mojom::BeijingPageHost> beijing_host_;

  std::unique_ptr<content::V8ValueConverter> v8_value_converter_;
  SEQUENCE_CHECKER(sequence_checker_);

  gin::WeakCellFactory<JSBeijingApi> weak_factory_{this};
};

}  // namespace beijing

#endif  // CHROME_RENDERER_BEIJING_JS_BEIJING_API_H_

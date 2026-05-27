// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BEIJING_BEIJING_PAGE_HOST_IMPL_H_
#define CHROME_BROWSER_BEIJING_BEIJING_PAGE_HOST_IMPL_H_

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "chrome/common/beijing/beijing_page_api.mojom.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace content {
class RenderFrameHost;
}

namespace beijing {

// window.beijing V8 注入在浏览器侧的一对一 Mojo 接收服务实现
class BeijingPageHostImpl : public mojom::BeijingPageHost {
 public:
  static void Create(content::RenderFrameHost* render_frame_host,
                     mojo::PendingReceiver<mojom::BeijingPageHost> receiver);

  BeijingPageHostImpl(const BeijingPageHostImpl&) = delete;
  BeijingPageHostImpl& operator=(const BeijingPageHostImpl&) = delete;

  ~BeijingPageHostImpl() override;

 private:
  explicit BeijingPageHostImpl(content::RenderFrameHost* render_frame_host);

  // mojom::BeijingPageHost:
  void Ping(PingCallback callback) override;
  void GetApiVersion(GetApiVersionCallback callback) override;
  void EchoObject(base::Value input, EchoObjectCallback callback) override;
  void WrapObjectWithBrowserMeta(base::Value input,
                                 WrapObjectWithBrowserMetaCallback callback) override;
  void Subscribe(
      const std::string& topic,
      mojo::PendingAssociatedRemote<mojom::BeijingPageClient> client) override;
  void Publish(const std::string& topic, base::Value payload) override;

  raw_ptr<content::RenderFrameHost> render_frame_host_;
};

}  // namespace beijing

#endif  // CHROME_BROWSER_BEIJING_BEIJING_PAGE_HOST_IMPL_H_

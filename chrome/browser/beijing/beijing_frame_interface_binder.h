// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BEIJING_BEIJING_FRAME_INTERFACE_BINDER_H_
#define CHROME_BROWSER_BEIJING_BEIJING_FRAME_INTERFACE_BINDER_H_

namespace content {
class RenderFrameHost;
}  // namespace content

namespace mojo {
template <typename>
class BinderMapWithContext;
}  // namespace mojo

namespace beijing {

// 向 BrowserInterfaceBroker 注册北京播放器相关的宿主接口接收服务
void PopulateBeijingFrameBinders(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map);

}  // namespace beijing

#endif  // CHROME_BROWSER_BEIJING_BEIJING_FRAME_INTERFACE_BINDER_H_

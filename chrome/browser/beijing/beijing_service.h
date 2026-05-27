// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BEIJING_BEIJING_SERVICE_H_
#define CHROME_BROWSER_BEIJING_BEIJING_SERVICE_H_

#include <map>
#include <string>

#include "base/no_destructor.h"
#include "base/values.h"
#include "chrome/common/beijing/beijing_page_api.mojom.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/remote_set.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace beijing {

// 浏览器进程北京 API 跨页面消息分发总线服务单例
class BeijingService {
 public:
  static BeijingService* GetInstance();

  BeijingService(const BeijingService&) = delete;
  BeijingService& operator=(const BeijingService&) = delete;

  // 注册订阅者
  void Subscribe(content::BrowserContext* context,
                 const std::string& topic,
                 mojo::PendingAssociatedRemote<mojom::BeijingPageClient> client);

  // 发布主题消息分发给同 BrowserContext 的所有订阅者
  void Publish(content::BrowserContext* context,
               const std::string& topic,
               const base::Value& payload);

 private:
  friend class base::NoDestructor<BeijingService>;
  BeijingService();
  ~BeijingService();

  // 根据 BrowserContext 隔离的主题订阅集合：context -> (topic -> AssociatedRemoteSet)
  std::map<content::BrowserContext*,
           std::map<std::string,
                    mojo::AssociatedRemoteSet<mojom::BeijingPageClient>>>
      subscribers_;
};

}  // namespace beijing

#endif  // CHROME_BROWSER_BEIJING_BEIJING_SERVICE_H_

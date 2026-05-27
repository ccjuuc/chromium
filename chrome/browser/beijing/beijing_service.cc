// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/beijing/beijing_service.h"

#include "content/public/browser/browser_context.h"

namespace beijing {

// static
BeijingService* BeijingService::GetInstance() {
  static base::NoDestructor<BeijingService> instance;
  return instance.get();
}

BeijingService::BeijingService() = default;

BeijingService::~BeijingService() = default;

void BeijingService::Subscribe(
    content::BrowserContext* context,
    const std::string& topic,
    mojo::PendingAssociatedRemote<mojom::BeijingPageClient> client) {
  if (!context) {
    return;
  }
  subscribers_[context][topic].Add(std::move(client));
}

void BeijingService::Publish(content::BrowserContext* context,
                             const std::string& topic,
                             const base::Value& payload) {
  if (!context) {
    return;
  }

  auto context_it = subscribers_.find(context);
  if (context_it == subscribers_.end()) {
    return;
  }

  auto topic_it = context_it->second.find(topic);
  if (topic_it == context_it->second.end()) {
    return;
  }

  for (auto& client : topic_it->second) {
    client->OnEvent(topic, payload.Clone());
  }
}

}  // namespace beijing

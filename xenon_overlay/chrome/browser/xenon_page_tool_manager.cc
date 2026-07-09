// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_page_tool_manager.h"

#include "content/public/browser/render_frame_host.h"

namespace xenon {

RegisteredTool::RegisteredTool(const std::string& name,
                               const std::string& description,
                               const std::string& input_schema,
                               mojo::PendingRemote<mojom::XenonToolExecutor> executor)
    : name(name),
      description(description),
      input_schema(input_schema),
      executor(std::move(executor)) {}

RegisteredTool::~RegisteredTool() = default;
RegisteredTool::RegisteredTool(RegisteredTool&&) = default;
RegisteredTool& RegisteredTool::operator=(RegisteredTool&&) = default;

XenonPageToolManager::XenonPageToolManager(content::RenderFrameHost* rfh)
    : content::DocumentUserData<XenonPageToolManager>(rfh) {}

XenonPageToolManager::~XenonPageToolManager() = default;

void XenonPageToolManager::RegisterTool(const std::string& name,
                                        const std::string& description,
                                        const std::string& input_schema,
                                        mojo::PendingRemote<mojom::XenonToolExecutor> executor) {
  // Erase existing tool with same name if any
  std::erase_if(tools_, [&](const auto& t) { return t.name == name; });
  tools_.emplace_back(name, description, input_schema, std::move(executor));
}

DOCUMENT_USER_DATA_KEY_IMPL(XenonPageToolManager);

}  // namespace xenon

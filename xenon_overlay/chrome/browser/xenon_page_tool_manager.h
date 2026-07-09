// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_PAGE_TOOL_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_PAGE_TOOL_MANAGER_H_

#include <string>
#include <vector>

#include "content/public/browser/document_user_data.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "xenon_overlay/public/mojom/xenon_page_api.mojom.h"

namespace xenon {

struct RegisteredTool {
  std::string name;
  std::string description;
  std::string input_schema;
  mojo::Remote<mojom::XenonToolExecutor> executor;

  RegisteredTool(const std::string& name,
                 const std::string& description,
                 const std::string& input_schema,
                 mojo::PendingRemote<mojom::XenonToolExecutor> executor);
  ~RegisteredTool();

  RegisteredTool(RegisteredTool&&);
  RegisteredTool& operator=(RegisteredTool&&);

  RegisteredTool(const RegisteredTool&) = delete;
  RegisteredTool& operator=(const RegisteredTool&) = delete;
};

class XenonPageToolManager : public content::DocumentUserData<XenonPageToolManager> {
 public:
  ~XenonPageToolManager() override;

  void RegisterTool(const std::string& name,
                    const std::string& description,
                    const std::string& input_schema,
                    mojo::PendingRemote<mojom::XenonToolExecutor> executor);

  const std::vector<RegisteredTool>& GetTools() const { return tools_; }

 private:
  explicit XenonPageToolManager(content::RenderFrameHost* rfh);
  friend class content::DocumentUserData<XenonPageToolManager>;
  DOCUMENT_USER_DATA_KEY_DECL();

  std::vector<RegisteredTool> tools_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_PAGE_TOOL_MANAGER_H_

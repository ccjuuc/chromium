// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/base_paths.h"
#include "base/check.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "mojo/core/embedder/configuration.h"
#include "mojo/core/embedder/embedder.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/views/views_test_suite.h"

namespace {

class GmViewsTestSuite : public views::ViewsTestSuite {
 public:
  using ViewsTestSuite::ViewsTestSuite;

 protected:
  void Initialize() override {
    views::ViewsTestSuite::Initialize();
    base::FilePath executable_dir;
    CHECK(base::PathService::Get(base::DIR_EXE, &executable_dir));
    const base::FilePath strings =
        executable_dir.AppendASCII("gen").AppendASCII("chrome").AppendASCII(
            "generated_resources_en-US.pak");
    CHECK(base::PathExists(strings));
    ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
        strings, ui::kScaleFactorNone);
    const base::FilePath policy_strings =
        executable_dir.AppendASCII("gen")
            .AppendASCII("components")
            .AppendASCII("strings")
            .AppendASCII("components_strings_en-US.pak");
    CHECK(base::PathExists(policy_strings));
    ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
        policy_strings, ui::kScaleFactorNone);
  }
};

}  // namespace

int main(int argc, char** argv) {
  GmViewsTestSuite suite(argc, argv);
  mojo::core::Configuration configuration;
  configuration.is_broker_process = true;
  mojo::core::Init(configuration);
  return suite.RunTests();
}

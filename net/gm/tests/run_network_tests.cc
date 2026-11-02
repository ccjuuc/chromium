// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/functional/bind.h"
#include "base/test/launcher/unit_test_launcher.h"
#include "mojo/core/embedder/embedder.h"
#include "net/test/net_test_suite.h"

int main(int argc, char** argv) {
  mojo::core::Configuration configuration;
  configuration.is_broker_process = true;
  mojo::core::Init(configuration);
  NetTestSuite suite(argc, argv);
  return base::LaunchUnitTests(
      argc, argv, base::BindOnce(&NetTestSuite::Run, base::Unretained(&suite)));
}

// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_manager.h"

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "build/buildflag.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/buildflags/buildflags.h"

namespace xenon {

TEST(XenonManagerTest, Ping_WhenDisconnected_ReturnsNotRunning) {
  base::test::TaskEnvironment task_environment;

  XenonManager* manager = XenonManager::GetInstance();
  manager->OnDisconnected();

  std::string reply;
  manager->Ping(base::BindOnce(
      [](std::string* out, const std::string& s) { *out = s; }, &reply));
  task_environment.RunUntilIdle();

  EXPECT_EQ(reply, "Error: Service not running.");
}

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
TEST(XenonManagerTest, PingAssociated_WhenDisconnected_ReturnsError) {
  base::test::TaskEnvironment task_environment;

  XenonManager* manager = XenonManager::GetInstance();
  manager->OnDisconnected();

  std::string reply;
  manager->PingAssociated(base::BindOnce(
      [](std::string* out, const std::string& s) { *out = s; }, &reply));
  task_environment.RunUntilIdle();

  EXPECT_EQ(reply, "Error: Xenon associated side not bound.");
}
#endif

}  // namespace xenon

// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_navigation.h"

#include "base/run_loop.h"
#include "base/test/test_future.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "net/base/net_errors.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {
using Reply = base::test::TestFuture<base::Value, std::string>;

class XenonElectronNavigationTest : public content::RenderViewHostTestHarness {
 protected:
  void Load(const GURL& url, Reply& reply) {
    LoadElectronURL(web_contents(),
                    content::NavigationController::LoadURLParams(url),
                    reply.GetCallback());
  }
  void Finish(const GURL& url) {
    content::WebContentsTester::For(web_contents())->TestDidFinishLoad(url);
  }
  void ExpectCode(Reply& reply, int error) {
    ASSERT_TRUE(reply.Wait());
    EXPECT_TRUE(reply.Get<1>().empty());
    ASSERT_TRUE(reply.Get<0>().is_dict());
    EXPECT_EQ(error, reply.Get<0>().GetDict().FindInt("errorCode"));
    EXPECT_EQ(error == net::OK, reply.Get<0>().GetDict().FindBool("ok"));
  }
};

TEST_F(XenonElectronNavigationTest, WaitsForCommittedDocumentLoad) {
  const GURL old_url("https://fixture.test/old");
  NavigateAndCommit(old_url);
  Reply reply;
  const GURL url("https://fixture.test/new");
  Load(url, reply);
  Finish(old_url);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(reply.IsReady());
  auto navigation = content::NavigationSimulator::CreateFromPending(
      web_contents()->GetController());
  navigation->SetKeepLoading(true);
  navigation->Commit();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(reply.IsReady());
  Finish(url);
  ExpectCode(reply, net::OK);
}

TEST_F(XenonElectronNavigationTest,
       NetworkFailureCannotBecomeErrorPageSuccess) {
  Reply reply;
  const GURL url("https://fixture.test/missing");
  Load(url, reply);
  auto navigation = content::NavigationSimulator::CreateFromPending(
      web_contents()->GetController());
  navigation->Fail(net::ERR_NAME_NOT_RESOLVED);
  navigation->CommitErrorPage();
  Finish(url);
  ExpectCode(reply, net::ERR_NAME_NOT_RESOLVED);
}

TEST_F(XenonElectronNavigationTest, ReplacingNavigationAbortsOnlyOldReply) {
  Reply first, second;
  Load(GURL("https://fixture.test/old"), first);
  const GURL url("https://fixture.test/new");
  Load(url, second);
  ExpectCode(first, net::ERR_ABORTED);
  EXPECT_FALSE(second.IsReady());
  auto navigation = content::NavigationSimulator::CreateFromPending(
      web_contents()->GetController());
  navigation->Commit();
  Finish(url);
  ExpectCode(second, net::OK);
}

TEST_F(XenonElectronNavigationTest, DestroyingContentsAbortsPendingReply) {
  Reply reply;
  Load(GURL("https://fixture.test/pending"), reply);
  DeleteContents();
  ExpectCode(reply, net::ERR_ABORTED);
}

TEST_F(XenonElectronNavigationTest, InvalidUrlCompletesAsynchronously) {
  Reply reply;
  Load(GURL("not a valid URL"), reply);
  EXPECT_FALSE(reply.IsReady());
  ExpectCode(reply, net::ERR_INVALID_URL);
}

TEST_F(XenonElectronNavigationTest, CommittedDocumentFailureRejects) {
  Reply reply;
  const GURL url("https://fixture.test/stopped");
  Load(url, reply);
  auto navigation = content::NavigationSimulator::CreateFromPending(
      web_contents()->GetController());
  navigation->SetKeepLoading(true);
  navigation->Commit();
  content::WebContentsTester::For(web_contents())
      ->TestDidFailLoadWithError(url, net::ERR_ABORTED);
  ExpectCode(reply, net::ERR_ABORTED);
}
}  // namespace
}  // namespace xenon::ipc

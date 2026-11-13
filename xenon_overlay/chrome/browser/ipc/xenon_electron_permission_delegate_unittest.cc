// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_delegate.h"

#include <memory>
#include <optional>

#include "base/functional/bind.h"
#include "content/public/browser/permission_controller_delegate.h"
#include "content/public/browser/permission_descriptor_util.h"
#include "content/public/browser/permission_request_description.h"
#include "content/public/browser/permission_status_subscription.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"

namespace xenon::ipc {
namespace {

using Result = content::PermissionResult;
using Status = blink::mojom::PermissionStatus;
using Descriptor = blink::mojom::PermissionDescriptorPtr;
using Id = content::PermissionController::SubscriptionId;

class NativeDelegate : public content::PermissionControllerDelegate {
 public:
  NativeDelegate() { SetSubscriptions(nullptr); }
  int queries = 0;
  int resets = 0;
  int subscriptions_added = 0;
  int subscriptions_removed = 0;
  GURL last_requesting;
  GURL last_embedding;
  std::optional<GURL> subscription_embedding_override;
  void RequestPermissionsFromCurrentDocument(
      content::RenderFrameHost*,
      const content::PermissionRequestDescription& description,
      base::OnceCallback<void(const std::vector<Result>&)> callback) override {
    ++queries;
    std::move(callback).Run(std::vector<Result>(description.permissions.size(),
                                                Result(Status::ASK)));
  }
  Status GetPermissionStatus(const Descriptor&,
                             const GURL& requesting,
                             const GURL& embedding) override {
    ++queries;
    last_requesting = requesting;
    last_embedding = embedding;
    return Status::ASK;
  }
  Result GetPermissionResultForOriginWithoutContext(
      const Descriptor&,
      const url::Origin& requesting,
      const url::Origin& embedding) override {
    ++queries;
    last_requesting = requesting.GetURL();
    last_embedding = embedding.GetURL();
    return Result(Status::ASK);
  }
  Result GetPermissionResultForCurrentDocument(const Descriptor&,
                                               content::RenderFrameHost*,
                                               bool) override {
    ++queries;
    return Result(Status::ASK);
  }
  Result GetPermissionResultForWorker(const Descriptor&,
                                      content::RenderProcessHost*,
                                      const GURL&) override {
    ++queries;
    return Result(Status::ASK);
  }
  Result GetPermissionResultForEmbeddedRequester(const Descriptor&,
                                                 content::RenderFrameHost*,
                                                 const url::Origin&) override {
    ++queries;
    return Result(Status::ASK);
  }
  void ResetPermission(blink::PermissionType,
                       const GURL& requesting,
                       const GURL& embedding) override {
    ++resets;
    last_requesting = requesting;
    last_embedding = embedding;
  }
  void OnPermissionStatusChangeSubscriptionAdded(Id id) override {
    ++subscriptions_added;
    ASSERT_TRUE(subscriptions());
    ASSERT_TRUE(subscriptions()->Lookup(id));
    subscriptions()->Lookup(id)->permission_result = Result(Status::ASK);
    if (subscription_embedding_override) {
      subscriptions()->Lookup(id)->embedding_origin =
          *subscription_embedding_override;
    }
  }
  void UnsubscribeFromPermissionResultChange(Id id) override {
    ++subscriptions_removed;
    ASSERT_TRUE(subscriptions()->Lookup(id));
  }
  std::optional<gfx::Rect> GetExclusionAreaBoundsInScreen(
      content::WebContents*) const override {
    return gfx::Rect(1, 2, 3, 4);
  }
  bool IsPermissionOverridable(blink::PermissionType,
                               base::optional_ref<const url::Origin>,
                               base::optional_ref<const url::Origin>) override {
    return false;
  }
};

class XenonElectronPermissionDelegateTest : public testing::Test {
 protected:
  void SetUp() override {
    ElectronSessionShutdownNotifierFactory::GetInstance();
    context_ = std::make_unique<content::TestBrowserContext>();
  }
  content::PermissionControllerDelegate* wrapper() {
    return GetElectronPermissionControllerDelegate(
        context_.get(),
        base::BindRepeating(
            [](NativeDelegate* delegate)
                -> content::PermissionControllerDelegate* { return delegate; },
            base::Unretained(&native_)));
  }
  content::BrowserTaskEnvironment environment_;
  NativeDelegate native_;
  std::unique_ptr<content::TestBrowserContext> context_;
};

TEST_F(XenonElectronPermissionDelegateTest, OrdinaryBrowserCallsStayNative) {
  auto* delegate = wrapper();
  EXPECT_EQ(delegate, wrapper());
  const auto descriptor = content::PermissionDescriptorUtil::
      CreatePermissionDescriptorForPermissionType(
          blink::PermissionType::NOTIFICATIONS);
  const GURL requesting("https://requesting.example/");
  const GURL embedding("https://embedding.example/");
  EXPECT_EQ(delegate->GetPermissionStatus(descriptor, requesting, embedding),
            Status::ASK);
  EXPECT_EQ(native_.last_requesting, requesting);
  EXPECT_EQ(native_.last_embedding, embedding);
  EXPECT_EQ(delegate
                ->GetPermissionResultForOriginWithoutContext(
                    descriptor, url::Origin::Create(requesting),
                    url::Origin::Create(embedding))
                .status,
            Status::ASK);
  EXPECT_EQ(
      delegate->GetPermissionResultForCurrentDocument(descriptor, nullptr, true)
          .status,
      Status::ASK);
  EXPECT_EQ(
      delegate->GetPermissionResultForWorker(descriptor, nullptr, requesting)
          .status,
      Status::ASK);
  EXPECT_EQ(delegate
                ->GetPermissionResultForEmbeddedRequester(
                    descriptor, nullptr, url::Origin::Create(requesting))
                .status,
            Status::ASK);
  bool requested = false;
  delegate->RequestPermissionsFromCurrentDocument(
      nullptr, content::PermissionRequestDescription(descriptor.Clone()),
      base::BindOnce(
          [](bool* called, const std::vector<Result>& values) {
            *called = true;
            ASSERT_EQ(values.size(), 1u);
            EXPECT_EQ(values[0].status, Status::ASK);
          },
          &requested));
  EXPECT_TRUE(requested);
  EXPECT_EQ(native_.queries, 6);
  delegate->ResetPermission(blink::PermissionType::NOTIFICATIONS, requesting,
                            embedding);
  EXPECT_EQ(native_.resets, 1);
  EXPECT_EQ(delegate->GetExclusionAreaBoundsInScreen(nullptr),
            gfx::Rect(1, 2, 3, 4));
  EXPECT_FALSE(delegate->IsPermissionOverridable(
      blink::PermissionType::NOTIFICATIONS, std::nullopt, std::nullopt));
}

TEST_F(XenonElectronPermissionDelegateTest,
       NativeEmbeddingOriginOverridesReachControllerSubscriptions) {
  auto* delegate = wrapper();
  content::PermissionController::SubscriptionsMap browser_subscriptions;
  delegate->SetSubscriptions(&browser_subscriptions);
  const auto id = Id::FromUnsafeValue(1);
  auto subscription = std::make_unique<content::PermissionResultSubscription>(
      content::PermissionDescriptorUtil::
          CreatePermissionDescriptorForPermissionType(
              blink::PermissionType::NOTIFICATIONS));
  subscription->requesting_origin = GURL("https://requesting.example/");
  subscription->embedding_origin = GURL("https://inner.example/");
  subscription->should_include_device_status = false;
  browser_subscriptions.AddWithID(std::move(subscription), id);

  // Chrome attributes some nested WebContents to their outer host. The native
  // delegate applies that override during subscription registration, and the
  // controller must receive it for subsequent permission override matching.
  native_.subscription_embedding_override = GURL("chrome://new-tab-page/");
  delegate->OnPermissionStatusChangeSubscriptionAdded(id);
  ASSERT_EQ(native_.subscriptions_added, 1);
  EXPECT_EQ(browser_subscriptions.Lookup(id)->embedding_origin,
            *native_.subscription_embedding_override);
  EXPECT_EQ(browser_subscriptions.Lookup(id)->permission_result->status,
            Status::ASK);
  delegate->UnsubscribeFromPermissionResultChange(id);
}

TEST_F(XenonElectronPermissionDelegateTest,
       NativeSubscriptionsUseAnIsolatedMap) {
  auto* delegate = wrapper();
  content::PermissionController::SubscriptionsMap browser_subscriptions;
  delegate->SetSubscriptions(&browser_subscriptions);
  const auto id = Id::FromUnsafeValue(1);
  const auto unregistered = Id::FromUnsafeValue(2);
  auto make_subscription = [] {
    auto subscription = std::make_unique<content::PermissionResultSubscription>(
        content::PermissionDescriptorUtil::
            CreatePermissionDescriptorForPermissionType(
                blink::PermissionType::NOTIFICATIONS));
    subscription->requesting_origin = GURL("https://requesting.example/");
    subscription->embedding_origin = GURL("https://embedding.example/");
    subscription->should_include_device_status = false;
    return subscription;
  };
  auto native = make_subscription();
  int notifications = 0;
  native->callback = base::BindRepeating(
      [](int* count, Result result, bool ignore_override) {
        ++*count;
        EXPECT_EQ(result.status, Status::DENIED);
        EXPECT_FALSE(ignore_override);
      },
      &notifications);
  browser_subscriptions.AddWithID(std::move(native), id);
  browser_subscriptions.AddWithID(make_subscription(), unregistered);
  delegate->OnPermissionStatusChangeSubscriptionAdded(id);
  EXPECT_EQ(native_.subscriptions_added, 1);
  ASSERT_NE(native_.subscriptions(), &browser_subscriptions);
  EXPECT_TRUE(native_.subscriptions()->Lookup(id));
  EXPECT_FALSE(native_.subscriptions()->Lookup(unregistered));
  ASSERT_TRUE(browser_subscriptions.Lookup(id)->permission_result);
  EXPECT_EQ(browser_subscriptions.Lookup(id)->permission_result->status,
            Status::ASK);
  native_.subscriptions()->Lookup(id)->callback.Run(Result(Status::DENIED),
                                                    false);
  EXPECT_EQ(notifications, 1);
  EXPECT_EQ(browser_subscriptions.Lookup(id)->permission_result->status,
            Status::DENIED);
  delegate->UnsubscribeFromPermissionResultChange(id);
  EXPECT_EQ(native_.subscriptions_removed, 1);
  EXPECT_FALSE(native_.subscriptions()->Lookup(id));
  delegate->SetSubscriptions(nullptr);
}

TEST(XenonElectronPermissionNamesTest,
     UsesElectronNamesAndCoversChromiumTypes) {
  using Type = blink::PermissionType;
  EXPECT_EQ(ElectronPermissionName(Type::MIDI_SYSEX), "midiSysex");
  EXPECT_EQ(ElectronPermissionName(Type::KEYBOARD_LOCK), "keyboardLock");
  EXPECT_EQ(ElectronPermissionName(Type::POINTER_LOCK), "pointerLock");
  EXPECT_EQ(ElectronPermissionName(Type::PROTECTED_MEDIA_IDENTIFIER),
            "mediaKeySystem");
  EXPECT_EQ(ElectronPermissionName(Type::AUDIO_CAPTURE), "media");
  EXPECT_EQ(ElectronPermissionName(Type::VIDEO_CAPTURE), "media");
  EXPECT_EQ(ElectronPermissionName(Type::CLIPBOARD_READ_WRITE),
            "clipboard-read");
  for (const auto type : blink::GetAllPermissionTypes()) {
    EXPECT_FALSE(ElectronPermissionName(type).empty())
        << static_cast<int>(type);
  }
  EXPECT_TRUE(ElectronPermissionName(Type::NUM).empty());
}

}  // namespace
}  // namespace xenon::ipc

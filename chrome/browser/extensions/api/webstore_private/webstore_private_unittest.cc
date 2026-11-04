// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <optional>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/json/json_reader.h"
#include "base/json/values_util.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/time/time.h"
#include "base/values.h"
#include "base/version_info/version_info.h"
#include "chrome/browser/extensions/extension_install_prompt_show_params.h"
#include "chrome/browser/extensions/extension_management.h"
#include "chrome/browser/extensions/test_extension_system.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/supervised_user/supervised_user_test_util.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "components/enterprise/browser/reporting/common_pref_names.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "extensions/browser/api/management/management_api.h"
#include "extensions/browser/api/webstore_private/webstore_private_api.h"
#include "extensions/browser/api_test_utils.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/event_router_factory.h"
#include "extensions/browser/extension_dialog_auto_confirm.h"
#include "extensions/browser/extension_function_constants.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/install_approval.h"
#include "extensions/browser/pref_names.h"
#include "extensions/buildflags/buildflags.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension_builder.h"
#include "extensions/common/extension_features.h"
#include "services/data_decoder/public/cpp/test_support/in_process_data_decoder.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"

#if !BUILDFLAG(IS_ANDROID)
#include "components/enterprise/promotion_types.h"
#endif  // !BUILDFLAG(IS_ANDROID)

static_assert(BUILDFLAG(ENABLE_EXTENSIONS_CORE));

namespace extensions {
namespace {
constexpr char kInvalidId[] = "Invalid id";
constexpr char kExtensionId[] = "abcdefghijklmnopabcdefghijklmnop";
constexpr char kFakeJustification[] = "I need it!";
constexpr char kExtensionManifest[] = R"({
  \"name\" : \"Extension\",
  \"manifest_version\": 3,
  \"version\": \"0.1\",
  \"permissions\": [ \"example.com\", \"cookies\"],
  \"optional_permissions\" : [\"notifications\"]})";

// The XL store can serve an unchanged Google package. Its enterprise policy
// identity remains the update URL in the package's manifest.
constexpr char kGoogleUpdateUrlManifest[] = R"({
  \"name\": \"Google update URL fixture\",
  \"manifest_version\": 3,
  \"version\": \"1.0\",
  \"update_url\": \"https://clients2.google.com/service/update2/crx\"})";
constexpr char kAllowGoogleUpdateUrlSettings[] = R"({
  "*": {"installation_mode": "blocked"},
  "update_url:https://clients2.google.com/service/update2/crx": {
    "installation_mode": "allowed"
  }
})";
constexpr char kXlUpdateUrlManifest[] = R"({
  \"name\": \"XL update URL fixture\",
  \"manifest_version\": 3,
  \"version\": \"1.0\",
  \"update_url\": \"https://xl.extensions.test.com/updates/test.xml\"})";
constexpr char kAllowXlUpdateUrlSettings[] = R"({
  "*": {"installation_mode": "blocked"},
  "update_url:https://xl.extensions.test.com/updates/test.xml": {
    "installation_mode": "allowed"
  }
})";
constexpr char kAllowStoreFallbacksSettings[] = R"({
  "*": {"installation_mode": "blocked"},
  "update_url:https://clients2.google.com/service/update2/crx": {
    "installation_mode": "allowed"
  },
  "update_url:https://xl.extensions.test.com/service/update2/crx": {
    "installation_mode": "allowed"
  }
})";

constexpr char kBlockAllExtensionSettings[] = R"({
  "*": {
    "installation_mode":"blocked",
    "blocked_install_message":"This extension is blocked."
  }
})";
constexpr char kBlockOneExtensionSettings[] = R"({
  "abcdefghijklmnopabcdefghijklmnop": {
    "installation_mode":"blocked"
  }
})";

constexpr char kBlockedManifestTypeExtensionSettings[] = R"({
  "*": {
    "allowed_types": ["theme", "hosted_app"]
  }
})";

constexpr char kBlockedCookiesPermissionsExtensionSettings[] = R"({
  "*": {
    "blocked_permissions": ["cookies"]
  }
})";

constexpr char kBlockedNotificationsPermissionsExtensionSettings[] = R"({
  "*": {
    "blocked_permissions": ["audio"]
  }
})";

constexpr char kWebstoreUserCancelledError[] = "User cancelled install";
constexpr char kWebstoreBlockByPolicy[] =
    "Extension installation is blocked by policy";

// Helper test struct used for holding data related to extension requests.
struct ExtensionRequestData {
  explicit ExtensionRequestData(base::Time timestamp)
      : ExtensionRequestData(timestamp, std::string()) {}
  ExtensionRequestData(base::Time timestamp, std::string justification_text)
      : timestamp(timestamp),
        justification_text(std::move(justification_text)) {}
  ~ExtensionRequestData() = default;

  base::Time timestamp;
  std::string justification_text;
};

// Verifies that the extension request pending list in |profile| matches the
// |expected_pending_requests|.
void VerifyPendingList(const std::map<ExtensionId, ExtensionRequestData>&
                           expected_pending_requests,
                       Profile* profile) {
  const base::DictValue& actual_pending_requests = profile->GetPrefs()->GetDict(
      enterprise_reporting::kCloudExtensionRequestIds);
  ASSERT_EQ(expected_pending_requests.size(), actual_pending_requests.size());
  for (const auto& expected_request : expected_pending_requests) {
    const base::DictValue* actual_pending_request =
        actual_pending_requests.FindDict(expected_request.first);
    ASSERT_NE(nullptr, actual_pending_request);

    // All extensions in the pending list are expected to have a timestamp.
    EXPECT_EQ(::base::TimeToValue(expected_request.second.timestamp),
              *actual_pending_request->Find(
                  extension_misc::kExtensionRequestTimestamp));

    // Extensions in the pending list may not have justification.
    if (!expected_request.second.justification_text.empty()) {
      EXPECT_EQ(expected_request.second.justification_text,
                *actual_pending_request->FindString(
                    extension_misc::kExtensionWorkflowJustification));
    } else {
      EXPECT_FALSE(actual_pending_request->contains(
          extension_misc::kExtensionWorkflowJustification));
    }
  }
}

void SetExtensionSettings(const std::string& settings_string,
                          TestingProfile* profile) {
  std::optional<base::Value> settings = base::JSONReader::Read(
      settings_string, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  ASSERT_TRUE(settings.has_value());
  profile->GetTestingPrefService()->SetManagedPref(
      pref_names::kExtensionManagement,
      base::Value::ToUniquePtrValue(std::move(*settings)));
}

std::unique_ptr<KeyedService> BuildManagementApi(
    content::BrowserContext* context) {
  return std::make_unique<ManagementAPI>(context);
}

std::unique_ptr<KeyedService> BuildEventRouter(
    content::BrowserContext* profile) {
  return std::make_unique<extensions::EventRouter>(
      profile, ExtensionPrefs::Get(profile));
}

}  // namespace

// TODO(crbug.com/408458901): Create a base test class for extensions tests that
// doesn't depend on ExtensionService and use it here.
class WebstorePrivateApiTestBase : public testing::Test {
 public:
  using ExtensionInstallStatus = api::webstore_private::ExtensionInstallStatus;
  WebstorePrivateApiTestBase() = default;
  WebstorePrivateApiTestBase(const WebstorePrivateApiTestBase&) = delete;
  WebstorePrivateApiTestBase& operator=(const WebstorePrivateApiTestBase&) =
      delete;

  // testing::Test:
  void SetUp() override {
    rvh_test_enabler_ = std::make_unique<content::RenderViewHostTestEnabler>();
    profile_manager_ = std::make_unique<TestingProfileManager>(
        TestingBrowserProcess::GetGlobal());
    ASSERT_TRUE(profile_manager_->SetUp());
    profile_ = profile_manager_->CreateTestingProfile(
        TestingProfile::kDefaultProfileUserName, /*prefs=*/nullptr,
        /*user_name=*/std::u16string(),
        /*avatar_id=*/0, /*testing_factories=*/{});
    CreateExtensionServiceAndSetFactories(profile());
    extension_ = ExtensionBuilder("Test").Build();
  }

  void CreateExtensionServiceAndSetFactories(Profile* profile) {
    TestExtensionSystem* extension_system =
        static_cast<TestExtensionSystem*>(ExtensionSystem::Get(profile));
    extension_system->CreateExtensionService(
        base::CommandLine::ForCurrentProcess(),
        base::FilePath() /* install_directory */,
        false /* autoupdate_enabled */);

    ManagementAPI::GetFactoryInstance()->SetTestingFactory(
        profile, base::BindRepeating(&BuildManagementApi));
    EventRouterFactory::GetInstance()->SetTestingFactory(
        profile, base::BindRepeating(&BuildEventRouter));

    // Create instance of ManagementAPI.
    CHECK(ManagementAPI::GetFactoryInstance()->Get(profile));
  }

  void TearDown() override {
    extension_ = nullptr;
    profile_ = nullptr;
    rvh_test_enabler_.reset();
  }

  std::string GenerateArgs(const char* id) {
    return base::StringPrintf(R"(["%s"])", id);
  }

  std::string GenerateArgs(const char* id, const char* manifest) {
    return base::StringPrintf(R"(["%s", "%s"])", id, manifest);
  }

  scoped_refptr<const Extension> CreateExtension(const ExtensionId& id) {
    return ExtensionBuilder("extension").SetID(id).Build();
  }

  std::optional<base::Value> RunFunctionAndReturnValue(
      scoped_refptr<ExtensionFunction> function,
      const std::string& args) {
    function->set_extension(extension_);
    return api_test_utils::RunFunctionAndReturnSingleResult(std::move(function),
                                                            args, profile());
  }

  std::string RunFunctionAndReturnError(
      scoped_refptr<ExtensionFunction> function,
      const std::string& args) {
    function->set_extension(extension_);
    return api_test_utils::RunFunctionAndReturnError(std::move(function), args,
                                                     profile());
  }

  void VerifyResponse(const ExtensionInstallStatus& expected_response,
                      const base::Value& actual_response) {
    ASSERT_TRUE(actual_response.is_string());
    EXPECT_EQ(ToString(expected_response), actual_response.GetString());
  }

  TestingProfileManager* profile_manager() { return profile_manager_.get(); }
  TestingProfile* profile() { return profile_.get(); }
  const Extension* extension() { return extension_.get(); }

 private:
  content::BrowserTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  std::unique_ptr<content::RenderViewHostTestEnabler> rvh_test_enabler_;
  std::unique_ptr<TestingProfileManager> profile_manager_;
  raw_ptr<TestingProfile> profile_ = nullptr;  // Owned by `profile_manager_`.
  scoped_refptr<const Extension> extension_;
};

TEST_F(WebstorePrivateApiTestBase, GetFullChromeVersion) {
  auto function =
      base::MakeRefCounted<WebstorePrivateGetFullChromeVersionFunction>();
  std::optional<base::Value> response =
      api_test_utils::RunFunctionAndReturnSingleResult(
          function.get(), /*args*/ "[]", profile());
  ASSERT_TRUE(response);
  ASSERT_TRUE(response->is_dict());

  std::string version = std::string(version_info::GetVersionNumber());
  EXPECT_EQ(version, *response->GetDict().FindString("version_number"));
}

class WebstorePrivateGetExtensionStatusTest
    : public WebstorePrivateApiTestBase {
 public:
  void SetUp() override {
    WebstorePrivateApiTestBase::SetUp();
    in_process_data_decoder_ =
        std::make_unique<data_decoder::test::InProcessDataDecoder>();
  }

 private:
  std::unique_ptr<data_decoder::test::InProcessDataDecoder>
      in_process_data_decoder_;
};

TEST_F(WebstorePrivateGetExtensionStatusTest, InvalidExtensionId) {
  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  EXPECT_EQ(kInvalidId,
            RunFunctionAndReturnError(function.get(),
                                      GenerateArgs("invalid-extension-id")));
}

TEST_F(WebstorePrivateGetExtensionStatusTest, ExtensionEnabled) {
  ExtensionRegistry::Get(profile())->AddEnabled(CreateExtension(kExtensionId));
  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  std::optional<base::Value> response =
      RunFunctionAndReturnValue(function.get(), GenerateArgs(kExtensionId));
  VerifyResponse(ExtensionInstallStatus::kEnabled, *response);
}

TEST_F(WebstorePrivateGetExtensionStatusTest, InvalidManifest) {
  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  EXPECT_EQ(
      "Invalid manifest",
      RunFunctionAndReturnError(
          function.get(), GenerateArgs(kExtensionId, "invalid-manifest")));
}

TEST_F(WebstorePrivateGetExtensionStatusTest, ExtensionBlockedByManifestType) {
  SetExtensionSettings(kBlockedManifestTypeExtensionSettings, profile());
  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  std::optional<base::Value> response = RunFunctionAndReturnValue(
      function.get(), GenerateArgs(kExtensionId, kExtensionManifest));
  VerifyResponse(ExtensionInstallStatus::kBlockedByPolicy, *response);
}

TEST_F(WebstorePrivateGetExtensionStatusTest, ExtensionBlockedByPermission) {
  SetExtensionSettings(kBlockedCookiesPermissionsExtensionSettings, profile());
  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  std::optional<base::Value> response = RunFunctionAndReturnValue(
      function.get(), GenerateArgs(kExtensionId, kExtensionManifest));
  VerifyResponse(ExtensionInstallStatus::kBlockedByPolicy, *response);
}

TEST_F(WebstorePrivateGetExtensionStatusTest,
       ExtensionBlockedWithRequestEnabled) {
  SetExtensionSettings(kBlockAllExtensionSettings, profile());
  profile()->GetTestingPrefService()->SetManagedPref(
      enterprise_reporting::kCloudExtensionRequestEnabled,
      std::make_unique<base::Value>(true));

  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  std::optional<base::Value> response = RunFunctionAndReturnValue(
      function.get(), GenerateArgs(kExtensionId, kExtensionManifest));
  VerifyResponse(ExtensionInstallStatus::kCanRequest, *response);
}

TEST_F(WebstorePrivateGetExtensionStatusTest,
       ExtensionNotBlockedByOptionalPermission) {
  SetExtensionSettings(kBlockedNotificationsPermissionsExtensionSettings,
                       profile());
  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  std::optional<base::Value> response = RunFunctionAndReturnValue(
      function.get(), GenerateArgs(kExtensionId, kExtensionManifest));
  VerifyResponse(ExtensionInstallStatus::kInstallable, *response);
}

TEST_F(WebstorePrivateGetExtensionStatusTest,
       XlStoreUsesGoogleManifestUpdateUrlPolicy) {
  SetExtensionSettings(kAllowGoogleUpdateUrlSettings, profile());
  // The Google origin is a positive control for the same manifest and policy.
  for (const char* source_url :
       {"https://chromewebstore.google.com/detail/test",
        "https://xl.extensions.test.com/detail/test"}) {
    SCOPED_TRACE(source_url);
    auto function =
        base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
    function->set_source_url(GURL(source_url));
    std::optional<base::Value> response = RunFunctionAndReturnValue(
        function.get(), GenerateArgs(kExtensionId, kGoogleUpdateUrlManifest));
    ASSERT_TRUE(response);
    VerifyResponse(ExtensionInstallStatus::kInstallable, *response);
  }
}

TEST_F(WebstorePrivateGetExtensionStatusTest,
       XlStoreUsesXmlManifestUpdateUrlPolicy) {
  SetExtensionSettings(kAllowXlUpdateUrlSettings, profile());
  struct TestCase {
    const char* source_url;
    ExtensionInstallStatus expected;
  };
  constexpr TestCase kTestCases[] = {
      {"https://chromewebstore.google.com/detail/test",
       ExtensionInstallStatus::kBlockedByPolicy},
      {"https://xl.extensions.test.com/detail/test",
       ExtensionInstallStatus::kInstallable}};
  for (const auto& test : kTestCases) {
    SCOPED_TRACE(test.source_url);
    auto function =
        base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
    function->set_source_url(GURL(test.source_url));
    auto response = RunFunctionAndReturnValue(
        function.get(), GenerateArgs(kExtensionId, kXlUpdateUrlManifest));
    ASSERT_TRUE(response);
    VerifyResponse(test.expected, *response);
  }
}

TEST_F(WebstorePrivateGetExtensionStatusTest,
       OnlyXlDistinguishesMissingAndEmptyManifestUpdateUrl) {
  struct TestCase {
    const char* source_url;
    const char* fallback_url;
    ExtensionInstallStatus known_manifest_status;
  };
  constexpr TestCase kTestCases[] = {
      {"https://chromewebstore.google.com/detail/test",
       "https://clients2.google.com/service/update2/crx",
       ExtensionInstallStatus::kInstallable},
      {"https://chrome.google.com/webstore/detail/test",
       "https://clients2.google.com/service/update2/crx",
       ExtensionInstallStatus::kInstallable},
      {"", "https://clients2.google.com/service/update2/crx",
       ExtensionInstallStatus::kInstallable},
      {"https://xl.extensions.test.com/detail/test",
       "https://xl.extensions.test.com/service/update2/crx",
       ExtensionInstallStatus::kBlockedByPolicy}};
  for (const auto& test : kTestCases) {
    SCOPED_TRACE(test.source_url);
    SetExtensionSettings(base::StringPrintf(R"({
      "*": {"installation_mode": "blocked"},
      "update_url:%s": {"installation_mode": "allowed"}
    })",
                                            test.fallback_url),
                         profile());
    auto unknown_manifest =
        base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
    unknown_manifest->set_source_url(GURL(test.source_url));
    auto response = RunFunctionAndReturnValue(unknown_manifest.get(),
                                              GenerateArgs(kExtensionId));
    ASSERT_TRUE(response);
    VerifyResponse(ExtensionInstallStatus::kInstallable, *response);

    auto known_manifest =
        base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
    known_manifest->set_source_url(GURL(test.source_url));
    response = RunFunctionAndReturnValue(
        known_manifest.get(), GenerateArgs(kExtensionId, kExtensionManifest));
    ASSERT_TRUE(response);
    VerifyResponse(test.known_manifest_status, *response);
  }
}

TEST_F(WebstorePrivateGetExtensionStatusTest,
       OnlyXlManifestlessRequestUsesInstalledManifestUpdateUrl) {
  SetExtensionSettings(kAllowXlUpdateUrlSettings, profile());
  ExtensionRegistry::Get(profile())->AddDisabled(
      ExtensionBuilder("extension")
          .SetID(kExtensionId)
          .SetManifestKey("update_url",
                          "https://xl.extensions.test.com/updates/test.xml")
          .Build());
  struct TestCase {
    const char* source_url;
    ExtensionInstallStatus expected;
  };
  constexpr TestCase kTestCases[] = {
      {"https://chromewebstore.google.com/detail/test",
       ExtensionInstallStatus::kBlockedByPolicy},
      {"https://chrome.google.com/webstore/detail/test",
       ExtensionInstallStatus::kBlockedByPolicy},
      {"https://xl.extensions.test.com/detail/test",
       ExtensionInstallStatus::kDisabled}};
  for (const auto& test : kTestCases) {
    SCOPED_TRACE(test.source_url);
    auto function =
        base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
    function->set_source_url(GURL(test.source_url));
    auto response =
        RunFunctionAndReturnValue(function.get(), GenerateArgs(kExtensionId));
    ASSERT_TRUE(response);
    VerifyResponse(test.expected, *response);
  }
}

TEST_F(WebstorePrivateGetExtensionStatusTest, ExtensionCorrupted) {
  ExtensionRegistry::Get(profile())->AddDisabled(CreateExtension(kExtensionId));
  ExtensionPrefs::Get(profile())->AddDisableReason(
      kExtensionId, disable_reason::DISABLE_CORRUPTED);
  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  std::optional<base::Value> response = RunFunctionAndReturnValue(
      function.get(), GenerateArgs(kExtensionId, kExtensionManifest));
  VerifyResponse(ExtensionInstallStatus::kCorrupted, *response);
}

class SupervisedUserWebstorePrivateGetExtensionStatusTest
    : public WebstorePrivateGetExtensionStatusTest {};

TEST_F(SupervisedUserWebstorePrivateGetExtensionStatusTest,
       ExtensionCustodianApprovalRequired) {
  profile()->SetIsSupervisedProfile(true);

  ExtensionRegistry::Get(profile())->AddDisabled(CreateExtension(kExtensionId));
  ExtensionPrefs::Get(profile())->AddDisableReason(
      kExtensionId, disable_reason::DISABLE_CUSTODIAN_APPROVAL_REQUIRED);
  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  std::optional<base::Value> response =
      RunFunctionAndReturnValue(function.get(), GenerateArgs(kExtensionId));
  VerifyResponse(ExtensionInstallStatus::kCustodianApprovalRequired, *response);
}

TEST_F(SupervisedUserWebstorePrivateGetExtensionStatusTest,
       ExtensionCustodianApprovalRequiredForInstallation) {
  profile()->SetIsSupervisedProfile(true);

  auto function =
      base::MakeRefCounted<WebstorePrivateGetExtensionStatusFunction>();
  std::optional<base::Value> response =
      RunFunctionAndReturnValue(function.get(), GenerateArgs(kExtensionId));

  ASSERT_FALSE(
      ExtensionRegistry::Get(profile())->GetInstalledExtension(kExtensionId));
  VerifyResponse(
      ExtensionInstallStatus::kCustodianApprovalRequiredForInstallation,
      *response);
}

class WebstorePrivateBeginInstallWithManifest3Test
    : public WebstorePrivateApiTestBase {
 public:
  WebstorePrivateBeginInstallWithManifest3Test() = default;

  void EnableExtensionRequest(bool enable) {
    profile()->GetTestingPrefService()->SetManagedPref(
        enterprise_reporting::kCloudExtensionRequestEnabled,
        std::make_unique<base::Value>(enable));
  }

  void SetExtensionSettings(const std::string& settings_string) {
    std::optional<base::Value> settings = base::JSONReader::Read(
        settings_string, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    ASSERT_TRUE(settings);
    profile()->GetTestingPrefService()->SetManagedPref(
        pref_names::kExtensionManagement,
        base::Value::ToUniquePtrValue(std::move(*settings)));
  }

  std::string GenerateArgs(const char* id, const char* manifest) {
    return base::StringPrintf(R"([{"id":"%s", "manifest":"%s"}])", id,
                              manifest);
  }

  void VerifyUserCancelledFunctionResult(ExtensionFunction* function) {
    ASSERT_TRUE(function->GetResultListForTest());
    const base::Value& result = (*function->GetResultListForTest())[0];
    EXPECT_EQ("user_cancelled", result.GetString());
    EXPECT_EQ(kWebstoreUserCancelledError, function->GetError());
  }

  void VerifyBlockedByPolicyFunctionResult(
      WebstorePrivateBeginInstallWithManifest3Function* function,
      const std::u16string& expected_blocked_message) {
    ASSERT_TRUE(function->GetResultListForTest());
    const base::Value& result = (*function->GetResultListForTest())[0];
    EXPECT_EQ("blocked_by_policy", result.GetString());
    EXPECT_EQ(kWebstoreBlockByPolicy, function->GetError());
    EXPECT_EQ(expected_blocked_message,
              function->GetBlockedByPolicyErrorMessageForTesting());
  }

  scoped_refptr<const Extension> CreateExtension(const ExtensionId& id) {
    return ExtensionBuilder("extension").SetID(id).Build();
  }

  void VerifySuccessfulBeginInstall(
      const char* manifest,
      const char* source_url = "https://xl.extensions.test.com/detail/test") {
    WebstorePrivateApi::ClearPendingApprovalsForTesting();
    auto web_contents =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    auto function = base::MakeRefCounted<
        WebstorePrivateBeginInstallWithManifest3Function>();
    function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
    function->set_source_url(GURL(source_url));
    ScopedTestDialogAutoConfirm auto_confirm(
        ScopedTestDialogAutoConfirm::ACCEPT);

    api_test_utils::RunFunction(
        function.get(), GenerateArgs(kExtensionId, manifest), profile());
    EXPECT_TRUE(function->GetError().empty()) << function->GetError();
    EXPECT_EQ(1, WebstorePrivateApi::GetPendingApprovalsCountForTesting());
    WebstorePrivateApi::ClearPendingApprovalsForTesting();
    ASSERT_TRUE(function->GetResultListForTest());
    ASSERT_FALSE(function->GetResultListForTest()->empty());
    const base::Value& result = (*function->GetResultListForTest())[0];
    ASSERT_TRUE(result.is_string());
    EXPECT_TRUE(result.GetString().empty()) << result.GetString();
  }

  void VerifyApprovalBoundToStore(const GURL& store_url,
                                  const GURL& other_store_url) {
    WebstorePrivateApi::ClearPendingApprovalsForTesting();
    auto web_contents =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    auto begin = base::MakeRefCounted<
        WebstorePrivateBeginInstallWithManifest3Function>();
    begin->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
    begin->set_source_url(store_url);
    {
      ScopedTestDialogAutoConfirm auto_confirm(
          ScopedTestDialogAutoConfirm::ACCEPT);
      auto response = RunFunctionAndReturnValue(
          begin, GenerateArgs(kExtensionId, kExtensionManifest));
      ASSERT_TRUE(response);
      ASSERT_TRUE(response->is_string());
      EXPECT_TRUE(response->GetString().empty());
    }
    ASSERT_EQ(1, WebstorePrivateApi::GetPendingApprovalsCountForTesting());

    GURL::Replacements different_port;
    different_port.SetPortStr("8443");
    for (const GURL& caller_url :
         {other_store_url, store_url.ReplaceComponents(different_port),
          GURL()}) {
      SCOPED_TRACE(caller_url.spec());
      auto complete =
          base::MakeRefCounted<WebstorePrivateCompleteInstallFunction>();
      complete->set_source_url(caller_url);
      EXPECT_EQ(
          base::StringPrintf(
              "%s does not match a previous call to beginInstallWithManifest3",
              kExtensionId),
          RunFunctionAndReturnError(
              complete,
              WebstorePrivateApiTestBase::GenerateArgs(kExtensionId)));
      EXPECT_EQ(1, WebstorePrivateApi::GetPendingApprovalsCountForTesting());
    }

    // A matching origin consumes the approval. Omit a sender frame so that the
    // test stops before starting a network download.
    auto complete =
        base::MakeRefCounted<WebstorePrivateCompleteInstallFunction>();
    complete->set_source_url(store_url);
    EXPECT_EQ(
        function_constants::kCouldNotFindSenderWebContents,
        RunFunctionAndReturnError(
            complete, WebstorePrivateApiTestBase::GenerateArgs(kExtensionId)));
    EXPECT_EQ(0, WebstorePrivateApi::GetPendingApprovalsCountForTesting());
  }

 private:
  // This test does not create a root window. Because of this,
  // ScopedDisableRootChecking needs to be used (which disables the root window
  // check).
  test::ScopedDisableRootChecking disable_root_checking_;
};

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       XlApprovalCannotBeCompletedByGoogleStore) {
  VerifyApprovalBoundToStore(GURL("https://xl.extensions.test.com/detail/test"),
                             GURL("https://chromewebstore.google.com/"));
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       GoogleApprovalCannotBeCompletedByXlStore) {
  VerifyApprovalBoundToStore(
      GURL("https://chromewebstore.google.com/detail/test"),
      GURL("https://xl.extensions.test.com/"));
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       XlStoreUsesGoogleManifestUpdateUrlPolicy) {
  SetExtensionSettings(kAllowGoogleUpdateUrlSettings);
  VerifySuccessfulBeginInstall(kGoogleUpdateUrlManifest);
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       XlStoreUsesXmlManifestUpdateUrlPolicy) {
  SetExtensionSettings(kAllowXlUpdateUrlSettings);
  VerifySuccessfulBeginInstall(kXlUpdateUrlManifest);
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       OnlyXlManifestWithoutUpdateUrlAvoidsStoreFallback) {
  SetExtensionSettings(kAllowStoreFallbacksSettings);
  WebstorePrivateApi::ClearPendingApprovalsForTesting();
  auto web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  function->set_source_url(GURL("https://xl.extensions.test.com/detail/test"));
  ScopedTestDialogAutoConfirm auto_confirm(ScopedTestDialogAutoConfirm::ACCEPT);
  api_test_utils::RunFunction(function.get(),
                              GenerateArgs(kExtensionId, kExtensionManifest),
                              profile());
  VerifyBlockedByPolicyFunctionResult(function.get(), std::u16string());
  EXPECT_EQ(0, WebstorePrivateApi::GetPendingApprovalsCountForTesting());
  function.reset();
  VerifySuccessfulBeginInstall(kExtensionManifest,
                               "https://chromewebstore.google.com/detail/test");
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       RequestExtensionWithConfirmThenShowPendingDialog) {
  EnableExtensionRequest(true);
  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);

  VerifyPendingList({}, profile());

  // Confirm request dialog
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  {
    ScopedTestDialogAutoConfirm auto_confirm(
        ScopedTestDialogAutoConfirm::ACCEPT);
    api_test_utils::RunFunction(function.get(),
                                GenerateArgs(kExtensionId, kExtensionManifest),
                                profile());
  }
  VerifyUserCancelledFunctionResult(function.get());
  VerifyPendingList({{kExtensionId, ExtensionRequestData(base::Time::Now())}},
                    profile());

  // Show pending request dialog which can only be canceled.
  function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  {
    ScopedTestDialogAutoConfirm auto_cancel(
        ScopedTestDialogAutoConfirm::CANCEL);
    api_test_utils::RunFunction(function.get(),
                                GenerateArgs(kExtensionId, kExtensionManifest),
                                profile());
  }
  VerifyUserCancelledFunctionResult(function.get());
  VerifyPendingList({{kExtensionId, ExtensionRequestData(base::Time::Now())}},
                    profile());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       RequestExtensionWithCancel) {
  EnableExtensionRequest(true);
  VerifyPendingList({}, profile());

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  ScopedTestDialogAutoConfirm auto_cancel(ScopedTestDialogAutoConfirm::CANCEL);

  api_test_utils::RunFunction(function.get(),
                              GenerateArgs(kExtensionId, kExtensionManifest),
                              profile());
  VerifyUserCancelledFunctionResult(function.get());
  VerifyPendingList({}, profile());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       RequestExtensionWithJustification) {
  EnableExtensionRequest(true);
  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);

  VerifyPendingList({}, profile());

  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  {
    ScopedTestDialogAutoConfirm auto_confirm(
        ScopedTestDialogAutoConfirm::ACCEPT);
    auto_confirm.set_justification(kFakeJustification);
    api_test_utils::RunFunction(function.get(),
                                GenerateArgs(kExtensionId, kExtensionManifest),
                                profile());
  }
  // Even though the ACCEPT button was selected above, the extension request
  // dialog results in user_cancelled.
  VerifyUserCancelledFunctionResult(function.get());
  VerifyPendingList({{kExtensionId, ExtensionRequestData(base::Time::Now(),
                                                         kFakeJustification)}},
                    profile());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       RequestExtensionWithJustificationAndCancel) {
  EnableExtensionRequest(true);
  VerifyPendingList({}, profile());

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  {
    ScopedTestDialogAutoConfirm auto_cancel(
        ScopedTestDialogAutoConfirm::CANCEL);
    auto_cancel.set_justification(kFakeJustification);

    api_test_utils::RunFunction(function.get(),
                                GenerateArgs(kExtensionId, kExtensionManifest),
                                profile());
  }
  VerifyUserCancelledFunctionResult(function.get());
  VerifyPendingList({}, profile());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       NormalInstallIfRequestExtensionIsDisabled) {
  EnableExtensionRequest(true);
  VerifyPendingList({}, profile());

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  {
    ScopedTestDialogAutoConfirm auto_confirm(
        ScopedTestDialogAutoConfirm::ACCEPT);
    api_test_utils::RunFunction(function.get(),
                                GenerateArgs(kExtensionId, kExtensionManifest),
                                profile());
  }
  VerifyPendingList({{kExtensionId, ExtensionRequestData(base::Time::Now())}},
                    profile());

  // Show install prompt dialog if extension request feature is disabled.
  EnableExtensionRequest(false);
  function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  {
    // Successfully confirm the install prompt and the API returns an empty
    // string without error.
    ScopedTestDialogAutoConfirm auto_cancel(
        ScopedTestDialogAutoConfirm::ACCEPT);
    std::optional<base::Value> response = RunFunctionAndReturnValue(
        function.get(), GenerateArgs(kExtensionId, kExtensionManifest));
    ASSERT_TRUE(response);
    ASSERT_TRUE(response->is_string());
    EXPECT_TRUE(response->GetString().empty());
  }

  // Pending list is not changed.
  VerifyPendingList({{kExtensionId, ExtensionRequestData(base::Time::Now())}},
                    profile());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       InvalidManifestVersionZero) {
  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());

  const char kInvalidManifest[] = R"({
    \"name\" : \"Extension\",
    \"manifest_version\": 0,
    \"version\": \"0.1\"
  })";

  ScopedTestDialogAutoConfirm auto_confirm(ScopedTestDialogAutoConfirm::ACCEPT);

  api_test_utils::RunFunction(
      function.get(), GenerateArgs(kExtensionId, kInvalidManifest), profile());

  EXPECT_EQ(ExtensionFunction::ResponseType::kFailed,
            *function->response_type());
  std::string error = function->GetError();
  EXPECT_TRUE(base::StartsWith(error, "Invalid manifest"));
  // Validate that the error includes more details than just a generic
  // "Invalid manifest" error. This matching will need to be updated if
  // the error is changed.
  EXPECT_TRUE(error.find("Invalid value for 'manifest_version'") !=
              std::string::npos);
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test, BlockedByPolicy) {
  SetExtensionSettings(kBlockAllExtensionSettings);

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  ScopedTestDialogAutoConfirm auto_confirm(ScopedTestDialogAutoConfirm::ACCEPT);

  api_test_utils::RunFunction(function.get(),
                              GenerateArgs(kExtensionId, kExtensionManifest),
                              profile());
  VerifyBlockedByPolicyFunctionResult(
      function.get(), u"From your administrator: This extension is blocked.");
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       BlockedByPolicyWithExtensionRequest) {
  SetExtensionSettings(kBlockOneExtensionSettings);
  EnableExtensionRequest(true);
  VerifyPendingList({}, profile());

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  ScopedTestDialogAutoConfirm auto_confirm(ScopedTestDialogAutoConfirm::ACCEPT);

  api_test_utils::RunFunction(function.get(),
                              GenerateArgs(kExtensionId, kExtensionManifest),
                              profile());
  VerifyPendingList({}, profile());
  VerifyBlockedByPolicyFunctionResult(function.get(), std::u16string());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       ExtensionBlockedByManifestType) {
  SetExtensionSettings(kBlockedManifestTypeExtensionSettings);

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  ScopedTestDialogAutoConfirm auto_confirm(ScopedTestDialogAutoConfirm::ACCEPT);

  api_test_utils::RunFunction(function.get(),
                              GenerateArgs(kExtensionId, kExtensionManifest),
                              profile());
  VerifyBlockedByPolicyFunctionResult(function.get(), std::u16string());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       ExtensionBlockedByPermission) {
  SetExtensionSettings(kBlockedCookiesPermissionsExtensionSettings);

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  ScopedTestDialogAutoConfirm auto_confirm(ScopedTestDialogAutoConfirm::ACCEPT);

  api_test_utils::RunFunction(function.get(),
                              GenerateArgs(kExtensionId, kExtensionManifest),
                              profile());
  VerifyBlockedByPolicyFunctionResult(function.get(), std::u16string());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       ExtensionNotBlockedByOptionalPermission) {
  SetExtensionSettings(kBlockedNotificationsPermissionsExtensionSettings);

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  ScopedTestDialogAutoConfirm auto_confirm(ScopedTestDialogAutoConfirm::ACCEPT);

  std::optional<base::Value> response = RunFunctionAndReturnValue(
      function.get(), GenerateArgs(kExtensionId, kExtensionManifest));
  // The API returns an empty string on success.
  ASSERT_TRUE(response);
  ASSERT_TRUE(response->is_string());
  EXPECT_EQ(std::string(), response->GetString());
}

TEST_F(WebstorePrivateBeginInstallWithManifest3Test,
       ProfileDeletedBeforeCompleteInstall) {
  const std::string profile_name = "deleted_before_complete_install";
  TestingProfile* const test_profile =
      profile_manager()->CreateTestingProfile(profile_name);
  ASSERT_TRUE(test_profile);
  CreateExtensionServiceAndSetFactories(test_profile);
  // There should be no pending approvals.
  EXPECT_EQ(WebstorePrivateApi::GetPendingApprovalsCountForTesting(), 0);
  {
    std::unique_ptr<content::WebContents> web_contents =
        content::WebContentsTester::CreateTestWebContents(test_profile,
                                                          nullptr);
    auto function = base::MakeRefCounted<
        WebstorePrivateBeginInstallWithManifest3Function>();
    function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
    ScopedTestDialogAutoConfirm auto_confirm(
        ScopedTestDialogAutoConfirm::ACCEPT);

    function->set_extension(extension());
    auto response = api_test_utils::RunFunctionAndReturnSingleResult(
        function.get(), GenerateArgs(kExtensionId, kExtensionManifest),
        test_profile);
    // The API returns an empty string on success.
    ASSERT_TRUE(response);
    ASSERT_TRUE(response->is_string());
    EXPECT_EQ(response->GetString(), "");
    // Running the function creates a pending approval.
    EXPECT_EQ(WebstorePrivateApi::GetPendingApprovalsCountForTesting(), 1);
  }
  // Deleting the Profile should remove the pending approval.
  profile_manager()->DeleteTestingProfile(profile_name);
  EXPECT_EQ(WebstorePrivateApi::GetPendingApprovalsCountForTesting(), 0);
}

struct FrictionDialogTestCase {
  const char* test_name;
  bool esb_user;
  const char* esb_allowlist;
  bool expected_friction_shown;
  ScopedTestDialogAutoConfirm::AutoConfirm dialog_action =
      ScopedTestDialogAutoConfirm::ACCEPT;
};

std::ostream& operator<<(std::ostream& out,
                         const FrictionDialogTestCase& test_case) {
  out << test_case.test_name;
  return out;
}

const FrictionDialogTestCase kFrictionDialogTestCases[] = {
    {/*test_name=*/"EsbUserAndNotAllowlisted",
     /*esb_user=*/true,
     /*esb_allowlist=*/"false",
     /*expected_friction_shown=*/true},

    {/*test_name=*/"EsbUserAndAllowlisted",
     /*esb_user=*/true,
     /*esb_allowlist=*/"true",
     /*expected_friction_shown=*/false},

    {/*test_name=*/"EsbUserAndUndefined",
     /*esb_user=*/true,
     /*esb_allowlist=*/"undefined",
     /*expected_friction_shown=*/false},
    {/*test_name=*/"NonEsbUserAndNotAllowlisted",
     /*esb_user=*/false,
     /*esb_allowlist=*/"false",
     /*expected_friction_shown=*/false},

    {/*test_name=*/"CancelFrictionDialog",
     /*esb_user=*/true,
     /*esb_allowlist=*/"false",
     /*expected_friction_shown=*/true,
     /*dialog_action=*/ScopedTestDialogAutoConfirm::CANCEL}};

class WebstorePrivateBeginInstallWithManifest3FrictionDialogTest
    : public WebstorePrivateBeginInstallWithManifest3Test,
      public testing::WithParamInterface<FrictionDialogTestCase> {
 public:
  WebstorePrivateBeginInstallWithManifest3FrictionDialogTest() = default;

  void SetUp() override {
    WebstorePrivateBeginInstallWithManifest3Test::SetUp();

    // Clear the pending approvals. Leftover approvals can stay pending when
    // testing the `webstorePrivate.beginInstallWithManifest3` function
    // without calling `webstorePrivate.completeInstall`.
    WebstorePrivateApi::ClearPendingApprovalsForTesting();
  }
};

TEST_P(WebstorePrivateBeginInstallWithManifest3FrictionDialogTest,
       FrictionDialogTests) {
  FrictionDialogTestCase test_case = GetParam();

  if (test_case.esb_user) {
    // Enable Enhanced Protection
    safe_browsing::SetSafeBrowsingState(
        profile()->GetPrefs(),
        safe_browsing::SafeBrowsingState::ENHANCED_PROTECTION);
  }

  std::unique_ptr<content::WebContents> web_contents =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto function =
      base::MakeRefCounted<WebstorePrivateBeginInstallWithManifest3Function>();
  function->SetRenderFrameHost(web_contents->GetPrimaryMainFrame());
  ScopedTestDialogAutoConfirm auto_confirm(test_case.dialog_action);

  std::string_view esb_allowlist = test_case.esb_allowlist;
  std::string args =
      esb_allowlist == "undefined"
          ? base::StringPrintf(R"([{"id":"%s", "manifest":"%s"}])",
                               kExtensionId, kExtensionManifest)
          : base::StringPrintf(
                R"([{"id":"%s", "manifest":"%s", "esbAllowlist":%s}])",
                kExtensionId, kExtensionManifest, esb_allowlist.data());

  if (test_case.dialog_action == ScopedTestDialogAutoConfirm::ACCEPT) {
    std::optional<base::Value> response =
        RunFunctionAndReturnValue(function.get(), args);

    // The API returns empty string when extension is installed successfully.
    ASSERT_TRUE(response);
    ASSERT_TRUE(response->is_string());
    EXPECT_EQ(std::string(), response->GetString());
  } else {
    api_test_utils::RunFunction(function.get(), args, profile());
    VerifyUserCancelledFunctionResult(function.get());
  }

  EXPECT_EQ(test_case.expected_friction_shown,
            function->GetFrictionDialogShownForTesting());

  std::unique_ptr<InstallApproval> approval =
      WebstorePrivateApi::PopApprovalForTesting(profile(), kExtensionId);
  if (test_case.dialog_action == ScopedTestDialogAutoConfirm::ACCEPT) {
    ASSERT_TRUE(approval);
    EXPECT_EQ(test_case.expected_friction_shown,
              approval->bypassed_safebrowsing_friction);
  } else {
    EXPECT_FALSE(approval);
  }
}

INSTANTIATE_TEST_SUITE_P(
    All,
    WebstorePrivateBeginInstallWithManifest3FrictionDialogTest,
    testing::ValuesIn(kFrictionDialogTestCases),
    [](const testing::TestParamInfo<FrictionDialogTestCase>& info) {
      return info.param.test_name;
    });

#if BUILDFLAG(ENABLE_EXTENSIONS)
// A test suite to be used with the MV2 deprecation experiments.
// NOTE: Android does not support MV2 deprecation experiments.
using WebstorePrivateManifestV2DeprecationUnitTest = WebstorePrivateApiTestBase;

// Tests the behavior of the webstorePrivate.getMV2DeprecationStatus() function.
TEST_F(WebstorePrivateManifestV2DeprecationUnitTest,
       TestGetMV2DeprecationStatus) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndDisableFeature(
      extensions_features::kExtensionManifestV2Support);
  auto function =
      base::MakeRefCounted<WebstorePrivateGetMV2DeprecationStatusFunction>();
  std::optional<base::Value> response =
      api_test_utils::RunFunctionAndReturnSingleResult(
          function.get(), /*args*/ "[]", profile());
  ASSERT_TRUE(response);
  EXPECT_EQ("hard_disable", *response);
}

TEST_F(WebstorePrivateManifestV2DeprecationUnitTest,
       TestGetMV2DeprecationStatusWithSupport) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      extensions_features::kExtensionManifestV2Support);
  auto function =
      base::MakeRefCounted<WebstorePrivateGetMV2DeprecationStatusFunction>();
  std::optional<base::Value> response =
      api_test_utils::RunFunctionAndReturnSingleResult(
          function.get(), /*args*/ "[]", profile());
  ASSERT_TRUE(response);
  EXPECT_EQ("inactive", *response);
}
#endif  // BUILDFLAG(ENABLE_EXTENSIONS)

#if !BUILDFLAG(IS_ANDROID)
class WebstorePrivateLogEnterprisePromoShownFunctionTest
    : public WebstorePrivateApiTestBase {
 protected:
  base::HistogramTester histogram_tester_;
};

TEST_F(WebstorePrivateLogEnterprisePromoShownFunctionTest,
       HistogramRecordedDisplay) {
  auto function =
      base::MakeRefCounted<WebstorePrivateLogEnterprisePromoShownFunction>();

  api_test_utils::RunFunction(function.get(), "[]", profile());

  histogram_tester_.ExpectUniqueSample(
      "Enterprise.CwsPromotionBannerEvent",
      static_cast<int>(enterprise::CwsPromotionBannerEvent::kDisplayed), 1);
}

class WebstorePrivateOnEnterprisePromoClickFunctionTest
    : public WebstorePrivateApiTestBase {
 protected:
  base::HistogramTester histogram_tester_;
};

TEST_F(WebstorePrivateOnEnterprisePromoClickFunctionTest,
       HistogramRecordedClick) {
  PrefService* prefs = profile()->GetPrefs();
  prefs->SetBoolean(pref_names::kHasDismissedEnterprisePromotion, false);
  auto function =
      base::MakeRefCounted<WebstorePrivateOnEnterprisePromoClickFunction>();

  api_test_utils::RunFunction(function.get(), "[]", profile());

  histogram_tester_.ExpectUniqueSample(
      "Enterprise.CwsPromotionBannerEvent",
      static_cast<int>(enterprise::CwsPromotionBannerEvent::kClicked), 1);
  EXPECT_TRUE(prefs->GetBoolean(pref_names::kHasDismissedEnterprisePromotion));
}
#endif  // !BUILDFLAG(IS_ANDROID)

}  // namespace extensions

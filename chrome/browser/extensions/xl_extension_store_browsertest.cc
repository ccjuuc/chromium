// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "chrome/browser/extensions/updater/extension_updater.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/chrome_test_utils.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "extensions/browser/extension_dialog_auto_confirm.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/install/crx_install_error.h"
#include "extensions/browser/install/sandboxed_unpacker_failure_reason.h"
#include "extensions/browser/install_verifier.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/manifest_handlers/manifest_url_handlers.h"
#include "extensions/common/switches.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "services/network/public/cpp/network_switches.h"

namespace extensions {
namespace {

constexpr char kXlExtensionId[] = "mcjbgmlghdgeocallnajjfbblpondljm";
constexpr char kGoogleExtensionId[] = "ngjobkbdodapjbbncmagbccommkggmnj";
constexpr char kXlUpdateManifestPath[] =
    "/updates/mcjbgmlghdgeocallnajjfbblpondljm.xml";
constexpr char kXlUpdateCrxPath[] =
    "/crx/mcjbgmlghdgeocallnajjfbblpondljm-update.crx";

}  // namespace

// ExtensionBrowserTest normally relaxes publisher, install and content
// verification. Use the standard browser fixture here so these installation
// tests exercise the production trust configuration without those overrides.
class XlExtensionStoreBrowserTest : public InProcessBrowserTest {
 public:
  XlExtensionStoreBrowserTest() {
    net::EmbeddedTestServer::ServerCertificateConfig cert_config;
    cert_config.dns_names = {"xl.extensions.test.com"};
    https_server_.SetSSLConfig(cert_config);
    https_server_.RegisterRequestHandler(base::BindRepeating(
        &XlExtensionStoreBrowserTest::HandleRequest, base::Unretained(this)));
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(switches::kExtensionsInstallVerification,
                                    "enforce_strict");
    ASSERT_TRUE(https_server_.InitializeAndListen());
    // Keep the actual trusted origin, including the standard HTTPS port, while
    // directing its connections to the embedded server's dynamically bound
    // port.
    command_line->AppendSwitchASCII(
        network::switches::kHostResolverRules,
        "MAP xl.extensions.test.com " +
            https_server_.host_port_pair().ToString());
  }

 protected:
  bool PrepareStoreFrontend() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    base::FilePath source_root;
    if (!base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &source_root)) {
      return false;
    }
    const base::FilePath public_dir =
        source_root.AppendASCII("tools/xl_extension_store/public");
    return base::ReadFileToString(public_dir.AppendASCII("index.html"),
                                  &frontend_html_) &&
           base::ReadFileToString(public_dir.AppendASCII("app.js"),
                                  &frontend_script_) &&
           base::ReadFileToString(public_dir.AppendASCII("style.css"),
                                  &frontend_style_);
  }

  content::EvalJsResult WaitForDetailButton(const std::string& label) {
    return content::EvalJs(chrome_test_utils::GetActiveWebContents(this),
                           content::JsReplace(R"(
          new Promise(resolve => {
            const ready = () => {
              const button = document.querySelector('#content button');
              return button && !button.disabled && button.textContent === $1;
            };
            if (ready()) {
              resolve(true);
              return;
            }
            const observer = new MutationObserver(() => {
              if (ready()) {
                observer.disconnect();
                resolve(true);
              }
            });
            observer.observe(document.querySelector('#content'), {
              subtree: true, attributes: true, childList: true,
              characterData: true,
            });
          });
        )",
                                              label));
  }

  content::EvalJsResult ClickDetailButton() {
    // EvalJs supplies a user gesture, as a real click does. The action must
    // immediately become busy before any native dialog completes.
    return content::EvalJs(chrome_test_utils::GetActiveWebContents(this), R"(
      (() => {
        const button = document.querySelector('#content button');
        button.click();
        return button.disabled;
      })();
    )");
  }

  bool PrepareXlUpdate(const std::string& crx_filename) {
    // All response data is populated before InstallFromXlStore starts accepting
    // connections, and remains immutable while the server's IO thread uses it.
    base::ScopedAllowBlockingForTesting allow_blocking;
    const base::FilePath crx_path = chrome_test_utils::GetChromeTestDataDir()
                                        .AppendASCII("extensions")
                                        .AppendASCII("xl_webstore")
                                        .AppendASCII(crx_filename);
    if (!base::ReadFileToString(crx_path, &update_crx_data_)) {
      return false;
    }
    const std::string codebase = extension_urls::GetXlWebstoreLaunchURL()
                                     .Resolve(kXlUpdateCrxPath)
                                     .spec();
    update_manifest_ =
        "<?xml version='1.0' encoding='UTF-8'?>"
        "<gupdate xmlns='http://www.google.com/update2/response' "
        "protocol='2.0'>"
        "<app appid='" +
        std::string(kXlExtensionId) + "'><updatecheck codebase='" + codebase +
        "' version='1.0.1'/></app></gupdate>";
    return true;
  }

  bool CheckForXlUpdate(std::optional<CrxInstallError>* install_error) {
    auto* updater = ExtensionUpdater::Get(profile());
    if (!updater || !updater->enabled()) {
      ADD_FAILURE() << "The production extension updater is not enabled";
      return false;
    }
    base::test::TestFuture<void> check_done;
    base::test::TestFuture<std::optional<CrxInstallError>> installer_done;
    updater->SetCrxInstallerResultCallbackForTesting(
        installer_done.GetCallback<const std::optional<CrxInstallError>&>());
    ExtensionUpdater::CheckParams params;
    params.ids = {kXlExtensionId};
    params.install_immediately = true;
    params.fetch_priority = DownloadFetchPriority::kForeground;
    params.callback = check_done.GetCallback();
    updater->CheckNow(std::move(params));
    if (!check_done.Wait() || !installer_done.IsReady()) {
      ADD_FAILURE() << "The update check did not attempt a CRX installation";
      return false;
    }
    *install_error = installer_done.Take();
    return true;
  }

  std::string InstallFromXlStore(const std::string& id,
                                 const std::string& manifest_filename,
                                 const base::FilePath& crx_relative_path) {
    const base::FilePath extensions_dir =
        chrome_test_utils::GetChromeTestDataDir().AppendASCII("extensions");
    std::string manifest;
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      if (!base::ReadFileToString(extensions_dir.AppendASCII("xl_webstore")
                                      .AppendASCII(manifest_filename),
                                  &manifest) ||
          !base::ReadFileToString(extensions_dir.Append(crx_relative_path),
                                  &crx_data_)) {
        ADD_FAILURE() << "Could not read signed CRX and manifest fixtures";
        return "fixture read failed";
      }
    }
    crx_path_ = "/crx/" + id + ".crx";
    if (!frontend_html_.empty()) {
      frontend_detail_path_ = "/detail/" + id;
      frontend_item_path_ = "/api/items/" + id;
      frontend_item_ =
          base::WriteJson(base::DictValue()
                              .Set("id", id)
                              .Set("name", "XL Store Test Extension")
                              .Set("description", "Store lifecycle fixture")
                              .Set("version", "1.0.0")
                              .Set("detailUrl", frontend_detail_path_)
                              .Set("manifest", manifest))
              .value();
    }
    // The handler and its immutable response data are ready before accepting
    // any requests from the browser or download service.
    https_server_.StartAcceptingConnections();

    content::WebContents* web_contents =
        chrome_test_utils::GetActiveWebContents(this);
    const GURL store_url =
        extension_urls::GetXlWebstoreLaunchURL().Resolve("empty.html");
    if (!content::NavigateToURL(web_contents, store_url)) {
      ADD_FAILURE() << "Could not navigate to the HTTPS XL store";
      return "navigation failed";
    }
    EXPECT_EQ(store_url, web_contents->GetLastCommittedURL());

    ScopedTestDialogAutoConfirm auto_confirm(
        ScopedTestDialogAutoConfirm::ACCEPT);
    // EvalJs executes in the page's main world with a user gesture. Only the
    // ordinary permission prompt is confirmed; no signature checks are skipped.
    const content::EvalJsResult result =
        content::EvalJs(web_contents, content::JsReplace(R"(
          new Promise(resolve => {
            const id = $1;
            chrome.webstorePrivate.beginInstallWithManifest3(
                {id, manifest: $2}, beginResult => {
                  const error = chrome.runtime.lastError;
                  if (error || beginResult !== '') {
                    resolve('begin: ' + (error ? error.message : beginResult));
                    return;
                  }
                  chrome.webstorePrivate.completeInstall(id, () => {
                    const error = chrome.runtime.lastError;
                    resolve(error ? 'complete: ' + error.message : 'success');
                  });
                });
          });
        )",
                                                         id, manifest));
    if (!result.is_string()) {
      ADD_FAILURE() << result;
      return "JavaScript failed";
    }
    return result.ExtractString();
  }

  Profile* profile() { return chrome_test_utils::GetProfile(this); }

 private:
  std::unique_ptr<net::test_server::HttpResponse> HandleRequest(
      const net::test_server::HttpRequest& request) {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    if (request.GetURL().path() == "/empty.html") {
      response->set_code(net::HTTP_OK);
      response->set_content_type("text/html");
      response->set_content(
          "<!doctype html><title>XL Store Install Test</title>");
      return response;
    }
    if (request.GetURL().path() == crx_path_) {
      response->set_code(net::HTTP_OK);
      response->set_content_type("application/x-chrome-extension");
      response->set_content(crx_data_);
      return response;
    }
    if (!frontend_html_.empty()) {
      const std::string path(request.GetURL().path());
      response->set_code(net::HTTP_OK);
      if (path == frontend_detail_path_) {
        response->set_content_type("text/html; charset=utf-8");
        response->set_content(frontend_html_);
        return response;
      }
      if (path == "/app.js") {
        response->set_content_type("text/javascript; charset=utf-8");
        response->set_content(frontend_script_);
        return response;
      }
      if (path == "/style.css") {
        response->set_content_type("text/css; charset=utf-8");
        response->set_content(frontend_style_);
        return response;
      }
      if (path == frontend_item_path_) {
        response->set_content_type("application/json");
        response->set_content(frontend_item_);
        return response;
      }
    }
    if (!update_manifest_.empty() &&
        request.GetURL().path() == kXlUpdateManifestPath) {
      response->set_code(net::HTTP_OK);
      response->set_content_type("application/xml");
      response->set_content(update_manifest_);
      return response;
    }
    if (!update_crx_data_.empty() &&
        request.GetURL().path() == kXlUpdateCrxPath) {
      response->set_code(net::HTTP_OK);
      response->set_content_type("application/x-chrome-extension");
      response->set_content(update_crx_data_);
      return response;
    }
    return nullptr;
  }

  // Keep update checks under explicit test control without changing
  // verification.
  ExtensionUpdater::ScopedSkipScheduledCheckForTest skip_scheduled_checks_;
  std::string crx_path_;
  std::string crx_data_;
  std::string update_manifest_;
  std::string update_crx_data_;
  std::string frontend_html_;
  std::string frontend_script_;
  std::string frontend_style_;
  std::string frontend_detail_path_;
  std::string frontend_item_path_;
  std::string frontend_item_;
  // Destroy the server before the response data and its bound fixture callback.
  net::EmbeddedTestServer https_server_{net::EmbeddedTestServer::TYPE_HTTPS};
};

IN_PROC_BROWSER_TEST_F(XlExtensionStoreBrowserTest,
                       InstallsXlPublisherPackage) {
  ASSERT_EQ("success", InstallFromXlStore(kXlExtensionId, "xl-manifest.json",
                                          base::FilePath(FILE_PATH_LITERAL(
                                              "xl_webstore/xl-signed.crx"))));
  const Extension* extension =
      ExtensionRegistry::Get(profile())->enabled_extensions().GetByID(
          kXlExtensionId);
  ASSERT_TRUE(extension);
  EXPECT_EQ("1.0.0", extension->version().GetString());
  EXPECT_TRUE(ExtensionPrefs::Get(profile())->IsFromXlWebstore(kXlExtensionId));
  EXPECT_FALSE(extension->from_webstore());
  EXPECT_TRUE(InstallVerifier::ShouldEnforce());
  EXPECT_FALSE(InstallVerifier::IsFromStore(*extension, profile()));
  EXPECT_FALSE(InstallVerifier::NeedsVerification(*extension, profile()));
  EXPECT_TRUE(extension_urls::IsXlWebstoreUpdateUrl(
      ManifestURL::GetUpdateURL(extension)));
}

IN_PROC_BROWSER_TEST_F(XlExtensionStoreBrowserTest,
                       InstallsOriginalGooglePublisherPackage) {
  ASSERT_EQ("success",
            InstallFromXlStore(kGoogleExtensionId, "google-manifest.json",
                               base::FilePath(FILE_PATH_LITERAL(
                                   "signin_screen_manual_test_extension/"
                                   "extension_signed_by_webstore.crx"))));
  const Extension* extension =
      ExtensionRegistry::Get(profile())->enabled_extensions().GetByID(
          kGoogleExtensionId);
  ASSERT_TRUE(extension);
  EXPECT_EQ("3.0", extension->version().GetString());
  EXPECT_TRUE(InstallVerifier::ShouldEnforce());
  EXPECT_TRUE(InstallVerifier::IsFromStore(*extension, profile()));
  EXPECT_FALSE(InstallVerifier::NeedsVerification(*extension, profile()));
  EXPECT_TRUE(
      ExtensionPrefs::Get(profile())->IsFromXlWebstore(kGoogleExtensionId));
  EXPECT_EQ(extension_urls::GetDefaultWebstoreUpdateUrl(),
            ManifestURL::GetUpdateURL(extension));
}

IN_PROC_BROWSER_TEST_F(XlExtensionStoreBrowserTest,
                       DetailPageUninstallsAndReinstallsWithoutReload) {
  ASSERT_TRUE(PrepareStoreFrontend());
  ASSERT_EQ("success", InstallFromXlStore(kXlExtensionId, "xl-manifest.json",
                                          base::FilePath(FILE_PATH_LITERAL(
                                              "xl_webstore/xl-signed.crx"))));
  auto* registry = ExtensionRegistry::Get(profile());
  auto* web_contents = chrome_test_utils::GetActiveWebContents(this);
  ASSERT_TRUE(content::NavigateToURL(
      web_contents, extension_urls::GetXlWebstoreLaunchURL().Resolve(
                        "/detail/" + std::string(kXlExtensionId))));
  ASSERT_EQ(true, WaitForDetailButton("卸载"));
  ASSERT_TRUE(content::ExecJs(web_contents, "window.storeTestDocument = {};"));

  {
    ScopedTestDialogAutoConfirm cancel(ScopedTestDialogAutoConfirm::CANCEL);
    ASSERT_EQ(true, ClickDetailButton());
    ASSERT_EQ(true, WaitForDetailButton("卸载"));
    ASSERT_TRUE(registry->GetInstalledExtension(kXlExtensionId));
    EXPECT_EQ(
        "已取消卸载。",
        content::EvalJs(
            web_contents,
            "document.querySelector('#content [role=status]').textContent"));
  }

  // Disabled extensions remain installed and must still offer removal.
  ASSERT_EQ("success",
            content::EvalJs(web_contents, content::JsReplace(R"(
    new Promise(resolve => chrome.management.setEnabled($1, false, () => {
      resolve(chrome.runtime.lastError?.message || 'success');
    }));
  )",
                                                             kXlExtensionId)));
  ASSERT_TRUE(registry->disabled_extensions().Contains(kXlExtensionId));
  ASSERT_EQ(true, WaitForDetailButton("卸载"));

  {
    ScopedTestDialogAutoConfirm accept(ScopedTestDialogAutoConfirm::ACCEPT);
    ASSERT_EQ(true, ClickDetailButton());
    ASSERT_EQ(true, WaitForDetailButton("添加至 Chrome"));
    ASSERT_FALSE(registry->GetInstalledExtension(kXlExtensionId));

    ASSERT_EQ(true, ClickDetailButton());
    ASSERT_EQ(true, WaitForDetailButton("卸载"));
    ASSERT_TRUE(registry->enabled_extensions().Contains(kXlExtensionId));

    // An uninstall outside the page's button must also update that same page.
    ASSERT_EQ("success", content::EvalJs(web_contents,
                                         content::JsReplace(R"(
      new Promise(resolve => chrome.management.uninstall(
          $1, {showConfirmDialog: true}, () => {
            resolve(chrome.runtime.lastError?.message || 'success');
          }));
    )",
                                                            kXlExtensionId)));
    ASSERT_EQ(true, WaitForDetailButton("添加至 Chrome"));
    EXPECT_FALSE(registry->GetInstalledExtension(kXlExtensionId));
  }
  EXPECT_EQ(true,
            content::EvalJs(web_contents,
                            "typeof window.storeTestDocument === 'object'"));
}

IN_PROC_BROWSER_TEST_F(XlExtensionStoreBrowserTest,
                       RejectsPackageWithoutPublisherProof) {
  const std::string result = InstallFromXlStore(
      kXlExtensionId, "xl-manifest.json",
      base::FilePath(FILE_PATH_LITERAL("xl_webstore/developer-only.crx")));
  EXPECT_EQ(0u, result.find("complete: ")) << result;
  EXPECT_NE(std::string::npos, result.find("CRX_REQUIRED_PROOF_MISSING"))
      << result;
  EXPECT_FALSE(
      ExtensionRegistry::Get(profile())->GetInstalledExtension(kXlExtensionId));
  EXPECT_FALSE(
      ExtensionPrefs::Get(profile())->IsFromXlWebstore(kXlExtensionId));
}

IN_PROC_BROWSER_TEST_F(XlExtensionStoreBrowserTest,
                       UpdatesXlPublisherPackageOverXmlProtocol) {
  ASSERT_TRUE(PrepareXlUpdate("xl-signed-1.0.1.crx"));
  ASSERT_EQ("success", InstallFromXlStore(kXlExtensionId, "xl-manifest.json",
                                          base::FilePath(FILE_PATH_LITERAL(
                                              "xl_webstore/xl-signed.crx"))));
  const Extension* original =
      ExtensionRegistry::Get(profile())->enabled_extensions().GetByID(
          kXlExtensionId);
  ASSERT_TRUE(original);
  ASSERT_EQ("1.0.0", original->VersionString());

  std::optional<CrxInstallError> install_error;
  ASSERT_TRUE(CheckForXlUpdate(&install_error));
  ASSERT_FALSE(install_error.has_value());
  const Extension* updated =
      ExtensionRegistry::Get(profile())->enabled_extensions().GetByID(
          kXlExtensionId);
  ASSERT_TRUE(updated);
  EXPECT_EQ("1.0.1", updated->VersionString());
  EXPECT_TRUE(ExtensionPrefs::Get(profile())->IsFromXlWebstore(kXlExtensionId));
  EXPECT_FALSE(updated->from_webstore());
  EXPECT_TRUE(InstallVerifier::ShouldEnforce());
  EXPECT_FALSE(InstallVerifier::IsFromStore(*updated, profile()));
  EXPECT_FALSE(InstallVerifier::NeedsVerification(*updated, profile()));
  EXPECT_TRUE(extension_urls::IsXlWebstoreUpdateUrl(
      ManifestURL::GetUpdateURL(updated)));
}

IN_PROC_BROWSER_TEST_F(XlExtensionStoreBrowserTest,
                       RejectsUpdateWithoutPublisherProof) {
  ASSERT_TRUE(PrepareXlUpdate("developer-only-1.0.1.crx"));
  ASSERT_EQ("success", InstallFromXlStore(kXlExtensionId, "xl-manifest.json",
                                          base::FilePath(FILE_PATH_LITERAL(
                                              "xl_webstore/xl-signed.crx"))));
  const Extension* before_update =
      ExtensionRegistry::Get(profile())->enabled_extensions().GetByID(
          kXlExtensionId);
  ASSERT_TRUE(before_update);
  ASSERT_EQ("1.0.0", before_update->VersionString());
  std::optional<CrxInstallError> install_error;
  ASSERT_TRUE(CheckForXlUpdate(&install_error));
  ASSERT_TRUE(install_error.has_value());
  ASSERT_EQ(CrxInstallErrorType::SANDBOXED_UNPACKER_FAILURE,
            install_error->type());
  EXPECT_EQ(SandboxedUnpackerFailureReason::CRX_REQUIRED_PROOF_MISSING,
            install_error->sandbox_failure_detail());
  const Extension* original =
      ExtensionRegistry::Get(profile())->enabled_extensions().GetByID(
          kXlExtensionId);
  ASSERT_TRUE(original);
  EXPECT_EQ("1.0.0", original->VersionString());
  EXPECT_TRUE(ExtensionPrefs::Get(profile())->IsFromXlWebstore(kXlExtensionId));
  EXPECT_FALSE(original->from_webstore());
  EXPECT_TRUE(InstallVerifier::ShouldEnforce());
  EXPECT_FALSE(InstallVerifier::IsFromStore(*original, profile()));
  EXPECT_FALSE(InstallVerifier::NeedsVerification(*original, profile()));
}

}  // namespace extensions

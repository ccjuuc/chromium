// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_info.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/gm/gm_key_manager.h"
#include "chrome/browser/gm/gm_policy.h"
#include "chrome/browser/gm/gm_system_trust_win.h"
#include "chrome/browser/net/profile_network_context_service.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/policy/core/browser/browser_policy_connector.h"
#include "components/policy/core/common/mock_configuration_policy_provider.h"
#include "components/policy/core/common/policy_map.h"
#include "components/policy/policy_constants.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/network_service_util.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/browser/service_process_info.h"
#include "content/public/browser/ssl_status.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_features.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "crypto/hash.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "net/base/net_errors.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "sandbox/policy/features.h"
#include "services/gm_key/test/key_service_test_client.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "ui/views/widget/widget.h"
#include "url/origin.h"

#if defined(GM_BROWSER_TEST_HAS_XENON_UPDATER)
#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"
#endif

namespace gm_browser {
namespace {

class GmIsolatedBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpInProcessBrowserTestFixture() override {
    InProcessBrowserTest::SetUpInProcessBrowserTestFixture();
#if defined(GM_BROWSER_TEST_HAS_XENON_UPDATER)
    // The product updater otherwise resolves the real HKCU/HKLM installation
    // during browser startup. Its existing test hooks keep all cleanup and
    // staging inside this fixture for the entire browser lifetime.
    ASSERT_TRUE(updater_install_directory_.CreateUniqueTempDir());
    auto* updater = xenon::updater::XenonUpdateManager::GetInstance();
    updater->SetSystemUpdaterForTesting(nullptr);
    updater->SetInstallDirForTesting(updater_install_directory_.GetPath());
    updater->SetInstallerPackageCacheRootForTesting(
        updater_install_directory_.GetPath().AppendASCII("packages"));
    updater->SetAutoDownload(false);
    updater->SetAutoInstallOnAppQuit(false);
#endif
  }

 private:
#if defined(GM_BROWSER_TEST_HAS_XENON_UPDATER)
  base::ScopedTempDir updater_install_directory_;
#endif
};

// Unlike the navigation fixture, this uses the registered production utility
// service and its normal launch options. No unsigned-loader hook is linked or
// enabled in that process.
class GmProductionKeyServiceBrowserTest : public GmIsolatedBrowserTest,
                                          public gm_key::mojom::KeyObserver {
 public:
  void TearDownOnMainThread() override {
    service_.reset();
    observers_.Clear();
  }

 protected:
  void LaunchService() {
    service_ = content::ServiceProcessHost::Launch<gm_key::mojom::KeyService>(
        content::ServiceProcessHost::Options()
            .WithDisplayName(u"USBKey production boundary test")
            .WithProcessCallback(base::BindOnce(
                &GmProductionKeyServiceBrowserTest::OnProcessLaunched,
                weak_factory_.GetWeakPtr()))
            .Pass());
  }

  mojo::PendingRemote<gm_key::mojom::KeyObserver> BindObserver() {
    mojo::PendingRemote<gm_key::mojom::KeyObserver> remote;
    observers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }

  mojo::Remote<gm_key::mojom::KeyService> service_;
  base::Process service_process_;

 private:
  void OnIdentitiesChanged() override {}
  void OnServiceFailure(gm_key::mojom::KeyError) override {}

  void OnProcessLaunched(const base::Process& process) {
    service_process_ = process.Duplicate();
  }
  mojo::ReceiverSet<gm_key::mojom::KeyObserver> observers_;
  base::WeakPtrFactory<GmProductionKeyServiceBrowserTest> weak_factory_{this};
};

IN_PROC_BROWSER_TEST_F(GmProductionKeyServiceBrowserTest,
                       ProductionUtilityRejectsUnsignedSimulator) {
  ASSERT_FALSE(base::CommandLine::ForCurrentProcess()->HasSwitch("no-sandbox"));
  base::FilePath directory;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &directory));
  auto config = gm_key::mojom::DriverConfig::New();
  config->library_path = directory.AppendASCII("gm_sm2_simulator.dll");
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    std::string bytes;
    ASSERT_TRUE(base::ReadFileToString(config->library_path, &bytes));
    const auto hash = crypto::hash::Sha256(bytes);
    config->sha256.assign(hash.begin(), hash.end());
  }
  auto repeated_config = config.Clone();
  LaunchService();
  ASSERT_TRUE(base::test::RunUntil([&] { return service_process_.IsValid(); }));
  EXPECT_NE(base::GetCurrentProcId(), service_process_.Pid());
  // The vendor SDK service is a dedicated fault-isolation process. Its mojom
  // declaration selects no sandbox without weakening the browser/network one.
  EXPECT_EQ(content::GetServiceSandboxType<gm_key::mojom::KeyService>(),
            sandbox::mojom::Sandbox::kNoSandbox);
  const auto integrity = base::GetProcessIntegrityLevel(service_process_.Pid());
  EXPECT_NE(base::INTEGRITY_UNKNOWN, integrity);
  EXPECT_GE(integrity, base::MEDIUM_INTEGRITY);

  base::test::TestFuture<gm_key::mojom::KeyResultPtr> configured;
  service_->Configure(std::move(config), BindObserver(),
                      configured.GetCallback());
  ASSERT_TRUE(configured.Wait());
  ASSERT_TRUE(configured.Get());
  EXPECT_EQ(gm_key::mojom::KeyError::kUntrustedDriver, configured.Get()->error);
  // A correct hash and nonzero WinTrust error identify signature rejection,
  // rather than an absent file, malformed config or hash mismatch.
  EXPECT_NE(0u, configured.Get()->system_error);

  base::test::TestFuture<gm_key::mojom::KeyResultPtr,
                         std::vector<gm_key::mojom::KeyIdentityPtr>>
      inventory;
  service_->Enumerate(inventory.GetCallback());
  ASSERT_TRUE(inventory.Wait());
  ASSERT_TRUE(inventory.Get<0>());
  EXPECT_EQ(gm_key::mojom::KeyError::kNotConfigured, inventory.Get<0>()->error);
  EXPECT_TRUE(inventory.Get<1>().empty());

  base::test::TestFuture<gm_key::mojom::KeyResultPtr> repeated;
  service_->Configure(std::move(repeated_config), BindObserver(),
                      repeated.GetCallback());
  ASSERT_TRUE(repeated.Wait());
  ASSERT_TRUE(repeated.Get());
  EXPECT_EQ(gm_key::mojom::KeyError::kAlreadyConfigured, repeated.Get()->error);
  EXPECT_TRUE(service_.is_connected());
}

struct BrowserCase {
  const char* name;
  bool tlcp;
  bool mutual;
  bool automatic = false;
  bool joint_tls13 = false;
};

// Keeps the real process-isolated SKF implementation, while allowing a test
// to deliver a successful SDK reply after its originating page was cancelled.
class ObservedKeyService final : public gm_key::mojom::KeyService {
 public:
  mojo::PendingRemote<gm_key::mojom::KeyService> Bind(
      mojo::PendingRemote<gm_key::mojom::KeyService> service) {
    service_.Bind(std::move(service));
    return receiver_.BindNewPipeAndPassRemote();
  }
  void Configure(gm_key::mojom::DriverConfigPtr config,
                 mojo::PendingRemote<gm_key::mojom::KeyObserver> observer,
                 ConfigureCallback callback) override {
    service_->Configure(std::move(config), std::move(observer),
                        std::move(callback));
  }
  void Enumerate(EnumerateCallback callback) override {
    service_->Enumerate(std::move(callback));
  }
  void Authorize(uint64_t request_id,
                 const std::string& identity_id,
                 uint64_t generation,
                 gm_key::SecurePin pin,
                 mojo::PendingReceiver<gm_key::mojom::KeySigner> signer,
                 AuthorizeCallback callback) override {
    authorizations.push_back(request_id);
    service_->Authorize(
        request_id, identity_id, generation, std::move(pin), std::move(signer),
        base::BindOnce(
            [](base::WeakPtr<ObservedKeyService> self,
               AuthorizeCallback callback, gm_key::mojom::KeyResultPtr result,
               gm_key::mojom::KeyIdentityPtr identity) {
              if (!self) {
                return;
              }
              if (self->hold_reply) {
                EXPECT_FALSE(self->held_reply);
                EXPECT_EQ(result->error, gm_key::mojom::KeyError::kOk);
                self->held_reply =
                    base::BindOnce(std::move(callback), std::move(result),
                                   std::move(identity));
              } else {
                std::move(callback).Run(std::move(result), std::move(identity));
              }
            },
            weak_factory_.GetWeakPtr(), std::move(callback)));
  }
  void CommitAuthorization(uint64_t request_id) override {
    commits.push_back(request_id);
    service_->CommitAuthorization(request_id);
  }
  void CancelAuthorization(uint64_t request_id) override {
    cancellations.push_back(request_id);
    service_->CancelAuthorization(request_id);
  }
  void GrantAuthorized(const std::string& identity_id,
                       uint64_t generation,
                       mojo::PendingReceiver<gm_key::mojom::KeySigner> signer,
                       GrantAuthorizedCallback callback) override {
    ++grants;
    service_->GrantAuthorized(identity_id, generation, std::move(signer),
                              std::move(callback));
  }
  void Lock(const std::string& identity_id, LockCallback callback) override {
    service_->Lock(identity_id, std::move(callback));
  }
  void Reset() {
    receiver_.reset();
    service_.reset();
    held_reply.Reset();
  }

  bool hold_reply = false;
  base::OnceClosure held_reply;
  std::vector<uint64_t> authorizations;
  std::vector<uint64_t> commits;
  std::vector<uint64_t> cancellations;
  int grants = 0;

 private:
  mojo::Remote<gm_key::mojom::KeyService> service_;
  mojo::Receiver<gm_key::mojom::KeyService> receiver_{this};
  base::WeakPtrFactory<ObservedKeyService> weak_factory_{this};
};

// Exercises production browser policy -> Profile KeyManager -> sandboxed
// NetworkService -> gm_backend. Only the native PIN UI and the hardware-facing
// utility launch are replaced. The separate helper performs real SM2 signing.
class GmBrowserTest : public GmIsolatedBrowserTest,
                      public testing::WithParamInterface<BrowserCase>,
                      public content::NetworkServiceProcessObserver {
 public:
  GmBrowserTest() {
    features_.InitWithFeatures(
        {sandbox::policy::features::kNetworkServiceSandbox},
        {features::kNetworkServiceInProcess});
  }

  void SetUpInProcessBrowserTestFixture() override {
    ASSERT_NO_FATAL_FAILURE(
        GmIsolatedBrowserTest::SetUpInProcessBrowserTestFixture());
    provider_.SetDefaultReturns(true, true);
    policy::BrowserPolicyConnector::SetPolicyProviderForTesting(&provider_);
  }

  void SetUpOnMainThread() override {
    content::AddNetworkServiceProcessObserver(this);
    observing_ = true;
    ASSERT_TRUE(content::IsOutOfProcessNetworkService());
    ASSERT_TRUE(SystemNetworkContextManager::IsNetworkSandboxEnabled());
    ASSERT_FALSE(
        base::CommandLine::ForCurrentProcess()->HasSwitch("no-sandbox"));
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_directory_));
    ASSERT_TRUE(fixtures_.CreateUniqueTempDir());
    client_root_ = fixtures_.GetPath().AppendASCII("simulator-root.der");
    base::CommandLine peer_command(executable_directory_.AppendASCII(
        "gm_interop_peer/gm_backend_smoke.exe"));
    peer_command.AppendArg("--serve");
    peer_command.AppendArg("0");
    peer_command.AppendArg(
        GetParam().tlcp
            ? (UseExplicitProtocolAlert() ? "tlcp-version-alert" : "tlcp")
            : "rfc8998-h2");
    peer_command.AppendArgPath(fixtures_.GetPath());
    if (GetParam().mutual) {
      peer_command.AppendArgPath(client_root_);
      peer_command.AppendArg("--wait-client-root");
    }
    base::LaunchOptions launch_options;
    launch_options.start_hidden = true;
    peer_ = base::LaunchProcess(peer_command, launch_options);
    ASSERT_TRUE(peer_.IsValid());
    const auto ready = fixtures_.GetPath().AppendASCII("ready");
    const auto deadline = base::TimeTicks::Now() + base::Seconds(15);
    while (!base::PathExists(ready) && base::TimeTicks::Now() < deadline) {
      base::PlatformThread::Sleep(base::Milliseconds(10));
    }
    std::string port_string;
    ASSERT_TRUE(base::ReadFileToString(ready, &port_string));
    base::TrimWhitespaceASCII(port_string, base::TRIM_ALL, &port_string);
    int port = 0;
    ASSERT_TRUE(base::StringToInt(port_string, &port));
    ASSERT_GT(port, 0);
    ASSERT_LE(port, 65535);
    url_ = GURL("https://localhost:" + port_string + "/");

    std::string server_root;
    ASSERT_TRUE(base::ReadFileToString(
        fixtures_.GetPath().AppendASCII("server-root.der"), &server_root));
    if (GetParam().automatic) {
      auto snapshot = network::mojom::GmAutoConfig::New();
      snapshot->enabled = true;
      snapshot->trust_anchors.emplace_back(server_root.begin(),
                                           server_root.end());
      SetGmSystemTrustSnapshotForTesting(std::move(snapshot));
      // Deliver the notification; FlushSSLConfigManagerForTesting below also
      // waits for the replacement snapshot before flushing its Mojo update.
      base::RunLoop().RunUntilIdle();
    }
    auto rule =
        base::DictValue()
            .Set("host", "localhost")
            .Set("port", port)
            .Set("protocol", GetParam().joint_tls13
                                 ? "tls13_auto"
                                 : (GetParam().tlcp ? "tlcp" : "rfc8998"))
            .Set("trust_profile_id", "browser-test-root")
            .Set("trust_generation", 1)
            .Set("policy_revision", 1)
            .Set("revocation", "disabled")
            .Set("trust_anchors",
                 base::ListValue().Append(base::Base64Encode(server_root)));
    policy::PolicyMap policies;
    if (GetParam().mutual && !GetParam().automatic) {
      rule.Set("identity_policy_id", "browser-test-key");
      rule.Set("identity_generation", 1);
    }
    {
      const auto driver =
          executable_directory_.AppendASCII("gm_sm2_simulator.dll");
      std::string driver_bytes;
      ASSERT_TRUE(base::ReadFileToString(driver, &driver_bytes));
      policies.Set(
          policy::key::kGmUsbKeyDriver, policy::POLICY_LEVEL_MANDATORY,
          policy::POLICY_SCOPE_MACHINE, policy::POLICY_SOURCE_PLATFORM,
          base::Value(
              base::DictValue()
                  .Set("path", driver.AsUTF8Unsafe())
                  .Set("sha256", base::HexEncodeLower(
                                     crypto::hash::Sha256(driver_bytes)))),
          nullptr);
      if (GetParam().mutual) {
        key_process_ = std::make_unique<gm_key::test::ServiceProcess>();
        ASSERT_TRUE(key_process_->Launch(executable_directory_.AppendASCII(
            "gm_key_service_test_helper.exe")));
      }
      auto* manager = ProfileNetworkContextServiceFactory::GetForContext(
                          browser()->GetProfile())
                          ->gm_key_manager_for_testing();
      ASSERT_TRUE(manager);
      manager->SetTestHooks(
          base::BindRepeating(&GmBrowserTest::TakeService,
                              base::Unretained(this)),
          base::BindRepeating(&GmBrowserTest::Prompt, base::Unretained(this)));
    }
    if (!GetParam().automatic) {
      policies.Set(policy::key::kGmTlsSiteRules, policy::POLICY_LEVEL_MANDATORY,
                   policy::POLICY_SCOPE_MACHINE, policy::POLICY_SOURCE_PLATFORM,
                   base::Value(base::ListValue().Append(std::move(rule))),
                   nullptr);
    }
    provider_.UpdateChromePolicy(policies);
    ASSERT_TRUE(base::test::RunUntil([&] {
      const auto* state = g_browser_process->local_state();
      return state->IsManagedPreference(kDriverPref) &&
             state->IsManagedPreference(kSiteRulesPref) ==
                 !GetParam().automatic &&
             ReadPolicy(state).sites.size() == (GetParam().automatic ? 0u : 1u);
    }));
    // SSLConfigClient is a separate Mojo pipe from NetworkContext. Flush the
    // policy update itself before navigation so cross-pipe ordering cannot
    // accidentally route this managed origin through ordinary BoringSSL.
    g_browser_process->system_network_context_manager()
        ->FlushSSLConfigManagerForTesting();
    browser()
        ->GetProfile()
        ->GetDefaultStoragePartition()
        ->FlushNetworkInterfaceForTesting();
    content::FlushNetworkServiceInstanceForTesting();
  }

  void TearDownOnMainThread() override {
    if (observing_) {
      content::RemoveNetworkServiceProcessObserver(this);
    }
    // Revoke browser grants and drain the network pipes while the simulator
    // still exists. Late favicon/connection callbacks must not re-enter a
    // test service factory whose process has already been torn down.
    provider_.UpdateChromePolicy(policy::PolicyMap());
    if (GetParam().automatic) {
      SetGmSystemTrustSnapshotForTesting(nullptr);
    }
    g_browser_process->system_network_context_manager()
        ->FlushSSLConfigManagerForTesting();
    CloseConnections();
    observed_service_.Reset();
    base::ScopedAllowBlockingForTesting allow_blocking;
    key_process_.reset();
    if (peer_.IsValid()) {
      EXPECT_TRUE(base::WriteFile(fixtures_.GetPath().AppendASCII("stop"), ""));
      // Connections may stay alive until browser teardown. Termination only
      // ends this test's own loopback peer after the bounded graceful wait.
      int exit_code = 0;
      if (!peer_.WaitForExitWithTimeout(base::Seconds(2), &exit_code)) {
        peer_.Terminate(0, true);
      }
    }
  }

 protected:
  virtual bool UseExplicitProtocolAlert() const { return GetParam().automatic; }

  void CheckNavigation() {
    auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
    content::TestNavigationObserver navigation(contents);
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url_));
    ASSERT_TRUE(navigation.last_navigation_succeeded())
        << net::ErrorToString(navigation.last_net_error_code()) << " at "
        << url_;
    EXPECT_EQ(
        "GM TLS connection verified",
        content::EvalJs(contents,
                        "document.querySelector('#gm-result').textContent"));
    EXPECT_EQ(GetParam().tlcp ? "http/1.1" : "h2",
              content::EvalJs(contents,
                              "performance.getEntriesByType('navigation')[0]"
                              ".nextHopProtocol"));
    auto* entry = contents->GetController().GetLastCommittedEntry();
    ASSERT_TRUE(entry);
    EXPECT_EQ(url_, entry->GetURL());
    const auto& ssl = entry->GetSSL();
    EXPECT_TRUE(ssl.initialized);
    ASSERT_TRUE(ssl.certificate);
    EXPECT_EQ(0u, ssl.cert_status);
    EXPECT_EQ(GetParam().tlcp ? net::SSL_CONNECTION_VERSION_TLCP
                              : net::SSL_CONNECTION_VERSION_TLS1_3,
              net::SSLConnectionStatusToVersion(ssl.connection_status));
    EXPECT_TRUE(network_service_sandboxed_);
    EXPECT_EQ(GetParam().mutual ? 1 : 0, prompts_);
    EXPECT_EQ(GetParam().mutual ? 1 : 0, service_launches_);
    EXPECT_EQ("1", content::EvalJs(contents,
                                   "document.querySelector('#gm-result')"
                                   ".dataset.clientHellos"));
    if (GetParam().mutual && navigations_ == 0) {
      const auto connection_id =
          content::EvalJs(contents,
                          "document.querySelector('#gm-result')"
                          ".dataset.connectionId")
              .ExtractString();
      EXPECT_TRUE(pending_connections_at_prompt_.contains(connection_id));
    }
    ++navigations_;
  }

  void CheckAutomaticRejection() {
    ASSERT_TRUE(GetParam().automatic);
    auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
    content::TestNavigationObserver navigation(contents);
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url_));
    EXPECT_FALSE(navigation.last_navigation_succeeded());
    EXPECT_EQ(net::ERR_SSL_PROTOCOL_ERROR, navigation.last_net_error_code());
    EXPECT_EQ(0, service_launches_);
    EXPECT_EQ(0, prompts_);
    ASSERT_NO_FATAL_FAILURE(CloseConnections());

    base::ScopedAllowBlockingForTesting allow_blocking;
    const auto count_files = [&](const base::FilePath::CharType* pattern) {
      base::FileEnumerator files(fixtures_.GetPath(), false,
                                 base::FileEnumerator::FILES, pattern);
      size_t count = 0;
      for (auto file = files.Next(); !file.empty(); file = files.Next()) {
        ++count;
      }
      return count;
    };
    const size_t ordinary_hellos =
        count_files(FILE_PATH_LITERAL("tls-client-hello-*"));
    EXPECT_GT(ordinary_hellos, 0u);
    // Count real peer-side handshakes, not just the resulting error page.
    // Ordinary connection retries are permitted; no connection may switch
    // to GM after this deliberately ambiguous failure signal.
    EXPECT_EQ(0u, count_files(FILE_PATH_LITERAL("tlcp-client-hello-*")));
    EXPECT_EQ(0u, count_files(FILE_PATH_LITERAL("identity-request-*")));
    EXPECT_EQ(ordinary_hellos,
              count_files(FILE_PATH_LITERAL("accepted-connection-*")));
  }

  void CheckOrdinaryHttps() {
    net::EmbeddedTestServer server(net::EmbeddedTestServer::TYPE_HTTPS);
    server.ServeFilesFromSourceDirectory("chrome/test/data");
    ASSERT_TRUE(server.Start());
    ASSERT_TRUE(
        ui_test_utils::NavigateToURL(browser(), server.GetURL("/title1.html")));
    auto* entry = browser()
                      ->GetTabStripModel()
                      ->GetActiveWebContents()
                      ->GetController()
                      .GetLastCommittedEntry();
    ASSERT_TRUE(entry);
    EXPECT_EQ(0u, entry->GetSSL().cert_status);
    EXPECT_NE(
        net::SSL_CONNECTION_VERSION_TLCP,
        net::SSLConnectionStatusToVersion(entry->GetSSL().connection_status));
    EXPECT_EQ(0, prompts_);
    EXPECT_EQ(0, service_launches_);
  }

  void CloseConnections() {
    base::test::TestFuture<void> closed;
    browser()
        ->GetProfile()
        ->GetDefaultStoragePartition()
        ->GetNetworkContext()
        ->CloseAllConnections(closed.GetCallback());
    ASSERT_TRUE(closed.Wait());
  }

  void CheckCancelledAuthorization(bool close_tab, bool sdk_reply_pending) {
    ASSERT_TRUE(GetParam().mutual);
    // Keep another tab alive so closing the requesting tab does not exit.
    {
      SCOPED_TRACE("Create the requesting tab");
      ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
          browser(), GURL("about:blank"),
          WindowOpenDisposition::NEW_FOREGROUND_TAB,
          ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
    }
    auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
    auto* manager = ProfileNetworkContextServiceFactory::GetForContext(
                        browser()->GetProfile())
                        ->gm_key_manager_for_testing();
    hold_pin_ = !sdk_reply_pending;
    observed_service_.hold_reply = sdk_reply_pending;
    {
      SCOPED_TRACE("Wait for the held PIN or SDK authorization reply");
      contents->GetController().LoadURLWithParams(
          content::NavigationController::LoadURLParams(url_));
      ASSERT_TRUE(base::test::RunUntil([&] {
        return sdk_reply_pending ? !!observed_service_.held_reply : !!held_pin_;
      }));
    }
    EXPECT_EQ(prompts_, 1);
    EXPECT_EQ(manager->active_web_contents_for_testing(), contents);
    EXPECT_TRUE(observed_service_.commits.empty());
    if (close_tab) {
      browser()->GetTabStripModel()->CloseWebContentsAt(
          browser()->GetTabStripModel()->active_index(), 0);
    } else {
      SCOPED_TRACE("Navigate away while GM authorization is pending");
      const GURL cancellation_url("about:blank");
      content::TestNavigationObserver navigation(contents);
      navigation.set_expected_initial_url(cancellation_url);
      // The UI helper waits for the old load to stop before starting a current
      // tab navigation. Here the old load must stay pending until this new
      // navigation cancels it.
      contents->GetController().LoadURLWithParams(
          content::NavigationController::LoadURLParams(cancellation_url));
      navigation.Wait();
      ASSERT_TRUE(navigation.last_navigation_succeeded())
          << net::ErrorToString(navigation.last_net_error_code());
      EXPECT_EQ(navigation.last_navigation_url(), cancellation_url);
    }
    {
      SCOPED_TRACE("Cancel authorization and discard its late reply");
      ASSERT_TRUE(base::test::RunUntil(
          [&] { return !manager->active_web_contents_for_testing(); }));
      if (sdk_reply_pending) {
        ASSERT_TRUE(base::test::RunUntil(
            [&] { return !observed_service_.cancellations.empty(); }));
        EXPECT_EQ(observed_service_.cancellations,
                  observed_service_.authorizations);
        std::move(observed_service_.held_reply).Run();
      } else {
        std::move(held_pin_).Run(
            0, std::make_unique<gm_key::SecurePin>(
                   std::vector<uint8_t>{'1', '2', '3', '4', '5', '6'}));
      }
      base::RunLoop().RunUntilIdle();
      EXPECT_TRUE(observed_service_.commits.empty());
      EXPECT_EQ(observed_service_.authorizations.size(),
                sdk_reply_pending ? 1u : 0u);
    }

    SCOPED_TRACE("Revisit GM HTTPS and obtain a fresh authorization");
    hold_pin_ = false;
    observed_service_.hold_reply = false;
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url_));
    auto* resumed_contents =
        browser()->GetTabStripModel()->GetActiveWebContents();
    EXPECT_EQ(
        "GM TLS connection verified",
        content::EvalJs(resumed_contents,
                        "document.querySelector('#gm-result').textContent"));
    EXPECT_EQ(prompts_, 2);
    EXPECT_EQ(service_launches_, 1);
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return observed_service_.commits.size() == 1; }));
    EXPECT_EQ(observed_service_.commits.front(),
              observed_service_.authorizations.back());
    EXPECT_EQ(observed_service_.grants, 0);
  }

 private:
  void OnServiceLaunched(const content::ServiceProcessInfo& info) override {
    const auto integrity =
        base::GetProcessIntegrityLevel(info.GetProcess().Pid());
    EXPECT_NE(base::INTEGRITY_UNKNOWN, integrity);
    network_service_sandboxed_ = integrity != base::INTEGRITY_UNKNOWN &&
                                 integrity < base::MEDIUM_INTEGRITY;
    EXPECT_TRUE(network_service_sandboxed_);
  }

  mojo::PendingRemote<gm_key::mojom::KeyService> TakeService() {
    ++service_launches_;
    if (!key_process_ || !key_process_->service().is_bound()) {
      ADD_FAILURE() << "The simulator service was unexpectedly restarted";
      return {};
    }
    return observed_service_.Bind(key_process_->service().Unbind());
  }

  base::WeakPtr<views::Widget> Prompt(const std::string& origin,
                                      const std::vector<std::u16string>& labels,
                                      const std::u16string& message,
                                      PinCallback callback) {
    ++prompts_;
    auto* manager = ProfileNetworkContextServiceFactory::GetForContext(
                        browser()->GetProfile())
                        ->gm_key_manager_for_testing();
    // Legacy profile broker requests have no page. This assertion proves that
    // actual navigation reached the new browser observer path.
    EXPECT_EQ(manager->active_web_contents_for_testing(),
              browser()->GetTabStripModel()->GetActiveWebContents());
    EXPECT_EQ(url::Origin::Create(url_).Serialize(), origin);
    EXPECT_FALSE(labels.empty());
    EXPECT_TRUE(message.empty());
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      base::FileEnumerator requests(fixtures_.GetPath(), false,
                                    base::FileEnumerator::FILES,
                                    FILE_PATH_LITERAL("identity-request-*"));
      for (auto file = requests.Next(); !file.empty(); file = requests.Next()) {
        pending_connections_at_prompt_.insert(
            file.BaseName().AsUTF8Unsafe().substr(
                std::string("identity-request-").size()));
      }
      EXPECT_FALSE(pending_connections_at_prompt_.empty());
    }
    key_process_->control()->Control(
        0, 0,
        base::BindOnce(&GmBrowserTest::OnClientRoot, base::Unretained(this),
                       std::move(callback)));
    return {};
  }

  void OnClientRoot(PinCallback callback,
                    gm_key::mojom::KeyResultPtr result,
                    const std::vector<uint8_t>& root) {
    const bool valid = result &&
                       result->error == gm_key::mojom::KeyError::kOk &&
                       !root.empty();
    EXPECT_TRUE(valid);
    if (!valid) {
      std::move(callback).Run(0, nullptr);
      return;
    }
    base::ScopedAllowBlockingForTesting allow_blocking;
    const auto temporary = client_root_.AddExtensionASCII("tmp");
    const bool published =
        base::WriteFile(temporary, root) && base::Move(temporary, client_root_);
    EXPECT_TRUE(published);
    if (!published) {
      std::move(callback).Run(0, nullptr);
      return;
    }
    if (hold_pin_) {
      held_pin_ = std::move(callback);
      return;
    }
    std::move(callback).Run(
        0, std::make_unique<gm_key::SecurePin>(
               std::vector<uint8_t>{'1', '2', '3', '4', '5', '6'}));
  }

  base::test::ScopedFeatureList features_;
  testing::NiceMock<policy::MockConfigurationPolicyProvider> provider_;
  base::ScopedTempDir fixtures_;
  base::FilePath executable_directory_;
  base::FilePath client_root_;
  base::Process peer_;
  std::unique_ptr<gm_key::test::ServiceProcess> key_process_;
  ObservedKeyService observed_service_;
  bool hold_pin_ = false;
  PinCallback held_pin_;
  GURL url_;
  bool observing_ = false;
  bool network_service_sandboxed_ = false;
  int prompts_ = 0;
  int service_launches_ = 0;
  int navigations_ = 0;
  std::set<std::string> pending_connections_at_prompt_;
};

IN_PROC_BROWSER_TEST_P(GmBrowserTest, NavigationAndReconnect) {
  ASSERT_NO_FATAL_FAILURE(CheckNavigation());
  ASSERT_NO_FATAL_FAILURE(CloseConnections());
  ASSERT_NO_FATAL_FAILURE(CheckNavigation());
}

IN_PROC_BROWSER_TEST_P(GmBrowserTest, OrdinaryHttpsNeverOpensKey) {
  ASSERT_NO_FATAL_FAILURE(CheckOrdinaryHttps());
}

class GmCancellationBrowserTest : public GmBrowserTest {};

IN_PROC_BROWSER_TEST_P(GmCancellationBrowserTest, ClosingPageDiscardsLatePin) {
  CheckCancelledAuthorization(true, false);
}

IN_PROC_BROWSER_TEST_P(GmCancellationBrowserTest, NavigationDiscardsLatePin) {
  CheckCancelledAuthorization(false, false);
}

IN_PROC_BROWSER_TEST_P(GmCancellationBrowserTest,
                       ClosingPageDiscardsLateSdkAuthorization) {
  CheckCancelledAuthorization(true, true);
}

IN_PROC_BROWSER_TEST_P(GmCancellationBrowserTest,
                       NavigationDiscardsLateSdkAuthorization) {
  CheckCancelledAuthorization(false, true);
}

INSTANTIATE_TEST_SUITE_P(
    NationalTls,
    GmCancellationBrowserTest,
    testing::Values(BrowserCase{"TlcpSimulatedKey", true, true},
                    BrowserCase{"Rfc8998SimulatedKey", false, true},
                    BrowserCase{"JointTls13SimulatedKey", false, true, false,
                                true}),
    [](const testing::TestParamInfo<BrowserCase>& info) {
      return info.param.name;
    });

class GmAutomaticRejectionBrowserTest : public GmBrowserTest {
 protected:
  bool UseExplicitProtocolAlert() const override { return false; }
};

IN_PROC_BROWSER_TEST_P(GmAutomaticRejectionBrowserTest,
                       GenericTlcpAlertStopsDiscovery) {
  ASSERT_NO_FATAL_FAILURE(CheckAutomaticRejection());
}

INSTANTIATE_TEST_SUITE_P(
    NationalTls,
    GmAutomaticRejectionBrowserTest,
    testing::Values(BrowserCase{"ServerAuth", true, false, true},
                    BrowserCase{"SimulatedKey", true, true, true}),
    [](const testing::TestParamInfo<BrowserCase>& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    NationalTls,
    GmBrowserTest,
    testing::Values(
        BrowserCase{"TlcpServerAuth", true, false},
        BrowserCase{"TlcpSimulatedKey", true, true},
        BrowserCase{"Rfc8998ServerAuth", false, false},
        BrowserCase{"Rfc8998SimulatedKey", false, true},
        BrowserCase{"JointTls13ServerAuth", false, false, false, true},
        BrowserCase{"JointTls13SimulatedKey", false, true, false, true},
        BrowserCase{"AutomaticTlcpServerAuth", true, false, true},
        BrowserCase{"AutomaticTlcpSimulatedKey", true, true, true}),
    [](const testing::TestParamInfo<BrowserCase>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace gm_browser

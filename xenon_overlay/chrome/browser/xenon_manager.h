#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_

#include "base/functional/callback.h"
#include "base/gtest_prod_util.h"
#include "base/memory/singleton.h"
#include "build/buildflag.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
#include "mojo/public/cpp/bindings/associated_remote.h"
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
#include "mojo/public/cpp/bindings/receiver.h"
#endif

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
#include "mojo/public/cpp/bindings/shared_remote.h"
#else
#include "mojo/public/cpp/bindings/remote.h"
#endif

namespace content {
class BrowserContext;
}  // namespace content

namespace xenon {

FORWARD_DECLARE_TEST(XenonManagerTest, Ping_WhenDisconnected_ReturnsNotRunning);
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
FORWARD_DECLARE_TEST(XenonManagerTest, PingAssociated_WhenDisconnected_ReturnsError);
#endif

// Singleton manager for the Xenon Service in the Browser process.
//
// Optional pieces (GN in xenon_overlay/buildflags/features.gni):
// - enable_xenon_manager_shared_remote — SharedRemote vs Remote for XenonMainService.
// - enable_xenon_associated_side — AssociatedRemote for XenonAssociatedSide + PingAssociated.
// - enable_xenon_browser_observer — XenonBrowserObserver receiver + SetBrowserObserver.
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
class XenonManager : public mojom::XenonBrowserObserver {
#else
class XenonManager {
#endif
 public:
  static XenonManager* GetInstance();

  void EnsureServiceStarted(content::BrowserContext* context);

  using PingCallback = base::OnceCallback<void(const std::string&)>;
  void Ping(PingCallback callback);

  // Drops the Utility process connection during browser shutdown (login gate
  // dismiss before any Browser window exists).
  void ShutdownForProcessExit();

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  void PingAssociated(PingCallback callback);
#endif

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  mojo::SharedRemote<mojom::XenonMainService> DuplicateServiceRemote() const {
    return service_remote_;
  }
#endif

#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  using ObserverEventTestCallback =
      base::OnceCallback<void(const std::string& message)>;

  void OnServiceEvent(const std::string& message) override;

  // WebUI: wait for the next Utility→Browser `OnServiceEvent` payload (tests
  // XenonBrowserObserver / SetBrowserObserver). Empty `message` means timeout,
  // disconnect, or no observer build.
  void CaptureNextObserverEventForTest(ObserverEventTestCallback callback);
#endif

 private:
  friend struct base::DefaultSingletonTraits<XenonManager>;
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest, Ping_WhenDisconnected_ReturnsNotRunning);
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest, PingAssociated_WhenDisconnected_ReturnsError);
#endif

  XenonManager();
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  ~XenonManager() override;
#else
  ~XenonManager();
#endif

  void OnDisconnected();
  void ResetServiceConnection();
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  void SetupAssociatedSide();
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  void SetupBrowserObserver();
#endif

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  mojo::SharedRemote<mojom::XenonMainService> service_remote_;
#else
  mojo::Remote<mojom::XenonMainService> service_remote_;
#endif

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  mojo::AssociatedRemote<mojom::XenonAssociatedSide> associated_side_remote_;
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  mojo::Receiver<mojom::XenonBrowserObserver> browser_observer_receiver_{this};
  ObserverEventTestCallback observer_event_test_callback_;
#endif
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_LOGIN_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_LOGIN_CONTROLLER_H_

#include <vector>

#include "base/callback_list.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/browser_list_observer.h"
#include "ui/gfx/geometry/rect.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"
#include "chrome/browser/ui/startup/startup_types.h"
#include "ui/views/widget/widget_observer.h"

class Browser;
class Profile;

namespace views {
class Widget;
}  // namespace views

namespace xenon {

// Gates main browser window creation until the Xenon WebUI login page calls
// SetAppSessionLoggedIn(true). Handles second-instance activation during login
// and optional relogin presentation when the session flag is cleared.
class XenonLoginController : public BrowserListObserver {
 public:
  static XenonLoginController* GetInstance();

  XenonLoginController(const XenonLoginController&) = delete;
  XenonLoginController& operator=(const XenonLoginController&) = delete;

  // StartupBrowserCreator hooks (return true if launch was deferred).
  bool MaybeDeferLaunchBrowserForLastProfiles(
      StartupBrowserCreator* creator,
      const base::CommandLine& command_line,
      const base::FilePath& cur_dir,
      chrome::startup::IsProcessStartup process_startup,
      chrome::startup::IsFirstRun is_first_run,
      StartupProfileInfo profile_info,
      const StartupBrowserCreator::Profiles& last_opened_profiles,
      bool restore_tabbed_browser);

  bool MaybeDeferSingleBrowserLaunch(
      StartupBrowserCreator* creator,
      const base::CommandLine& command_line,
      Profile* profile,
      const base::FilePath& cur_dir,
      chrome::startup::IsProcessStartup process_startup,
      chrome::startup::IsFirstRun is_first_run,
      bool restore_tabbed_browser);

  // Process singleton: if user launches another instance while login is
  // pending, activate the login UI and suppress normal command-line handling.
  bool HandleSecondProcessDuringLogin();

  void SetAppSessionLoggedIn(Profile* profile, bool logged_in);

  // Invoked when the login WebDialog finishes (user closed window or navigate).
  void NotifyLoginDialogClosed();

  static bool IsLoginGateEnabled(const base::CommandLine& command_line);
  static bool IsSessionLoggedIn(Profile* profile);
  static int GetReloginPresentation(Profile* profile);

 private:
  friend class base::NoDestructor<XenonLoginController>;

  XenonLoginController();
  ~XenonLoginController() override;

  void ShowLoginDialog(Profile* profile, bool is_relogin);
  void ActivateLoginWidget();
  void ResumePendingLaunch();
  void RestoreHiddenBrowsers();

  void EnsureBrowserListObserving();
  void StopBrowserListObserving();
  void ReparentLoginWidgetToBrowser(Browser* browser);
  void LayoutLoginWidgetOverAnchorFrame();
  void ObserveAnchorFrameWidget(views::Widget* frame_widget);
  void StopAnchorFrameObservation();
  void FinishLoginDialogClosed(bool logged_in_at_close,
                               bool had_deferred_launch);
  void DeferredPostLoginCloseCleanup(bool had_deferred_launch);

  // BrowserListObserver:
  void OnBrowserSetLastActive(Browser* browser) override;
  void OnBrowserRemoved(Browser* browser) override;

  void StopLoginWidgetObservation();
  void MaybeReparentLoginWidgetToActiveBrowser();
  void OnLoginWidgetActivationChanged(views::Widget* widget, bool active);
  void OnClosingAllBrowsers(bool closing);
  void OnAppTerminatingClosingLogin();
  void CloseLoginWidgetForProcessExit();

  class LoginWidgetObserver : public views::WidgetObserver {
   public:
    explicit LoginWidgetObserver(XenonLoginController* controller);
    ~LoginWidgetObserver() override;

    LoginWidgetObserver(const LoginWidgetObserver&) = delete;
    LoginWidgetObserver& operator=(const LoginWidgetObserver&) = delete;

    void OnWidgetActivationChanged(views::Widget* widget, bool active) override;

   private:
    raw_ptr<XenonLoginController> controller_;
  };

  class AnchorFrameWidgetObserver : public views::WidgetObserver {
   public:
    explicit AnchorFrameWidgetObserver(XenonLoginController* controller);
    ~AnchorFrameWidgetObserver() override;

    AnchorFrameWidgetObserver(const AnchorFrameWidgetObserver&) = delete;
    AnchorFrameWidgetObserver& operator=(const AnchorFrameWidgetObserver&) =
        delete;

    void OnWidgetBoundsChanged(views::Widget* widget,
                               const gfx::Rect& new_bounds) override;
    void OnWidgetDestroying(views::Widget* widget) override;

   private:
    raw_ptr<XenonLoginController> controller_;
  };

  base::OnceClosure pending_resume_launch_;
  std::vector<raw_ptr<Browser, VectorExperimental>> hidden_browsers_;
  raw_ptr<views::Widget> login_widget_ = nullptr;
  raw_ptr<Profile> login_profile_ = nullptr;
  raw_ptr<Browser> login_anchor_browser_ = nullptr;
  bool login_ui_open_ = false;
  bool browser_list_observation_active_ = false;
  bool suppress_login_close_cleanup_ = false;
  base::CallbackListSubscription closing_all_browsers_subscription_;
  base::CallbackListSubscription app_terminating_subscription_;
  LoginWidgetObserver login_widget_observer_impl_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      login_widget_observation_;

  AnchorFrameWidgetObserver anchor_frame_observer_impl_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      anchor_frame_observation_;

  base::WeakPtrFactory<XenonLoginController> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_LOGIN_CONTROLLER_H_

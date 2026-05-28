#include "xenon_overlay/chrome/browser/xenon_login_controller.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/current_thread.h"
#include "build/build_config.h"
#include "chrome/browser/background/extensions/background_mode_manager.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/headless/headless_mode_util.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/lifetime/application_lifetime_desktop.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/lifetime/termination_notification.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_thread.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/chrome/browser/xenon_prefs.h"

namespace xenon {

namespace {

constexpr char kDisableXenonLoginGate[] = "disable-xenon-login-gate";

// Defaults must match `XenonWebDialog::ShowForLogin` width/height.
constexpr int kLoginDialogWidth = 520;
constexpr int kLoginDialogHeight = 640;

GURL LoginPageUrl() {
  return XenonWebDialog::GetXenonLoginWebUIUrl();
}

bool ShouldSkipForCommandLine(const base::CommandLine& command_line) {
  if (command_line.HasSwitch(kDisableXenonLoginGate)) {
    return true;
  }
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  if (headless::IsHeadlessMode()) {
    return true;
  }
#endif
  if (StartupBrowserCreator::ShouldLoadProfileWithoutWindow(command_line)) {
    return true;
  }
  return false;
}

// Picks the Browser window to parent relogin dialogs: same logical session as
// |session_profile| (original profile), preferring the last globally active
// browser when it matches (covers incognito + multi-window), else last-active
// for the on-the-record profile, else any browser for that original profile.
Browser* FindBrowserToParentLoginDialog(Profile* session_profile) {
  if (!session_profile) {
    return nullptr;
  }
  Profile* const original = session_profile->GetOriginalProfile();

  if (Browser* last_active = chrome::FindLastActive()) {
    if (last_active->profile()->GetOriginalProfile() == original &&
        last_active->window()) {
      return last_active;
    }
  }

  if (Browser* b = chrome::FindLastActiveWithProfile(original)) {
    if (b->window()) {
      return b;
    }
  }

  for (Browser* browser : *BrowserList::GetInstance()) {
    if (browser->profile()->GetOriginalProfile() == original && browser->window()) {
      return browser;
    }
  }
  return nullptr;
}

}  // namespace

// static
XenonLoginController* XenonLoginController::GetInstance() {
  static base::NoDestructor<XenonLoginController> instance;
  return instance.get();
}

XenonLoginController::XenonLoginController()
    : closing_all_browsers_subscription_(chrome::AddClosingAllBrowsersCallback(
          base::BindRepeating(&XenonLoginController::OnClosingAllBrowsers,
                              base::Unretained(this)))),
      app_terminating_subscription_(
          browser_shutdown::AddAppTerminatingCallback(base::BindOnce(
              &XenonLoginController::OnAppTerminatingClosingLogin,
              base::Unretained(this)))),
      login_widget_observer_impl_(this),
      login_widget_observation_(&login_widget_observer_impl_),
      anchor_frame_observer_impl_(this),
      anchor_frame_observation_(&anchor_frame_observer_impl_) {}

XenonLoginController::LoginWidgetObserver::LoginWidgetObserver(
    XenonLoginController* controller)
    : controller_(controller) {}

XenonLoginController::LoginWidgetObserver::~LoginWidgetObserver() = default;

void XenonLoginController::LoginWidgetObserver::OnWidgetActivationChanged(
    views::Widget* widget,
    bool active) {
  controller_->OnLoginWidgetActivationChanged(widget, active);
}

void XenonLoginController::LoginWidgetObserver::OnWidgetDestroying(
    views::Widget* widget) {
  controller_->OnLoginWidgetDestroying(widget);
}

XenonLoginController::AnchorFrameWidgetObserver::AnchorFrameWidgetObserver(
    XenonLoginController* controller)
    : controller_(controller) {}

XenonLoginController::AnchorFrameWidgetObserver::~AnchorFrameWidgetObserver() =
    default;

void XenonLoginController::AnchorFrameWidgetObserver::OnWidgetBoundsChanged(
    views::Widget* widget,
    const gfx::Rect& new_bounds) {
  controller_->LayoutLoginWidgetOverAnchorFrame();
}

void XenonLoginController::AnchorFrameWidgetObserver::OnWidgetDestroying(
    views::Widget* widget) {
  controller_->StopAnchorFrameObservation();
}

XenonLoginController::~XenonLoginController() {
  StopAnchorFrameObservation();
  StopLoginWidgetObservation();
  StopBrowserListObserving();
}

// static
bool XenonLoginController::IsLoginGateEnabled(
    const base::CommandLine& command_line) {
  return !ShouldSkipForCommandLine(command_line);
}

// static
bool XenonLoginController::IsSessionLoggedIn(Profile* profile) {
  if (!profile) {
    return false;
  }
  profile = profile->GetOriginalProfile();
  return profile->GetPrefs()->GetBoolean(prefs::kAppSessionLoggedIn);
}

// static
int XenonLoginController::GetReloginPresentation(Profile* profile) {
  if (!profile) {
    return 0;
  }
  return profile->GetOriginalProfile()->GetPrefs()->GetInteger(
      prefs::kReloginPresentation);
}

void XenonLoginController::RestoreHiddenBrowsers() {
  for (Browser* browser : hidden_browsers_) {
    if (browser && browser->window()) {
      browser->window()->Show();
    }
  }
  hidden_browsers_.clear();
}

void XenonLoginController::EnsureBrowserListObserving() {
  if (browser_list_observation_active_) {
    return;
  }
  BrowserList::AddObserver(this);
  browser_list_observation_active_ = true;
}

void XenonLoginController::StopBrowserListObserving() {
  if (!browser_list_observation_active_) {
    return;
  }
  BrowserList::RemoveObserver(this);
  browser_list_observation_active_ = false;
}

void XenonLoginController::StopLoginWidgetObservation() {
  login_widget_observation_.Reset();
}

void XenonLoginController::StopAnchorFrameObservation() {
  anchor_frame_observation_.Reset();
}

void XenonLoginController::ObserveAnchorFrameWidget(
    views::Widget* frame_widget) {
  StopAnchorFrameObservation();
  if (frame_widget && login_widget_) {
    anchor_frame_observation_.Observe(frame_widget);
  }
}

void XenonLoginController::LayoutLoginWidgetOverAnchorFrame() {
  if (!login_widget_ || !login_anchor_browser_ ||
      !login_anchor_browser_->window()) {
    return;
  }
  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(login_anchor_browser_);
  if (!browser_view) {
    return;
  }
  views::Widget* parent_widget = browser_view->GetWidget();
  if (!parent_widget) {
    return;
  }
  gfx::Rect parent_bounds = parent_widget->GetWindowBoundsInScreen();
  gfx::Size dialog_size = login_widget_->GetWindowBoundsInScreen().size();
  if (dialog_size.IsEmpty()) {
    dialog_size = login_widget_->GetRootView()->GetPreferredSize();
  }
  if (dialog_size.IsEmpty()) {
    dialog_size = gfx::Size(kLoginDialogWidth, kLoginDialogHeight);
  }
  const int x =
      parent_bounds.x() + (parent_bounds.width() - dialog_size.width()) / 2;
  const int y =
      parent_bounds.y() + (parent_bounds.height() - dialog_size.height()) / 2;
  login_widget_->SetBoundsConstrained(
      gfx::Rect(x, y, dialog_size.width(), dialog_size.height()));
  login_widget_->StackAboveWidget(parent_widget);
}

void XenonLoginController::MaybeReparentLoginWidgetToActiveBrowser() {
  if (!login_widget_ || !login_profile_) {
    return;
  }
  for (Browser* browser : *BrowserList::GetInstance()) {
    if (!browser || !browser->window()) {
      continue;
    }
    if (browser->profile()->GetOriginalProfile() !=
        login_profile_->GetOriginalProfile()) {
      continue;
    }
    if (browser->window()->IsActive()) {
      ReparentLoginWidgetToBrowser(browser);
      return;
    }
  }
  if (Browser* b = FindBrowserToParentLoginDialog(login_profile_)) {
    ReparentLoginWidgetToBrowser(b);
  }
}

void XenonLoginController::ReparentLoginWidgetToBrowser(Browser* browser) {
  if (!login_widget_ || !browser || !browser->window()) {
    return;
  }
  if (login_anchor_browser_ == browser) {
    LayoutLoginWidgetOverAnchorFrame();
    return;
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (!browser_view) {
    return;
  }
  views::Widget* frame_widget = browser_view->GetWidget();
  if (!frame_widget) {
    return;
  }
  StopAnchorFrameObservation();
  login_widget_->Reparent(frame_widget);
  login_anchor_browser_ = browser;
  ObserveAnchorFrameWidget(frame_widget);
  LayoutLoginWidgetOverAnchorFrame();
}

void XenonLoginController::OnBrowserSetLastActive(Browser* browser) {
  if (!login_widget_ || !login_profile_ || !browser) {
    return;
  }
  if (browser->profile()->GetOriginalProfile() !=
      login_profile_->GetOriginalProfile()) {
    return;
  }
  ReparentLoginWidgetToBrowser(browser);
}

void XenonLoginController::OnLoginWidgetActivationChanged(views::Widget* widget,
                                                          bool active) {
  if (widget != login_widget_ || active) {
    return;
  }
  // Defer until activation / last-active browser state matches foreground.
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<XenonLoginController> self) {
            if (self) {
              self->MaybeReparentLoginWidgetToActiveBrowser();
            }
          },
          weak_factory_.GetWeakPtr()));
}

void XenonLoginController::OnBrowserRemoved(Browser* browser) {
  if (!login_widget_ || !login_profile_ || !browser) {
    return;
  }
  if (browser->profile()->GetOriginalProfile() != login_profile_) {
    return;
  }
  if (login_anchor_browser_ == browser) {
    login_anchor_browser_ = nullptr;
  }
  Browser* next = FindBrowserToParentLoginDialog(login_profile_);
  if (next) {
    ReparentLoginWidgetToBrowser(next);
  }
}

void XenonLoginController::ActivateLoginWidget() {
  if (login_widget_) {
    login_widget_->Activate();
    login_widget_->Show();
    LayoutLoginWidgetOverAnchorFrame();
  }
}

void XenonLoginController::OnClosingAllBrowsers(bool closing) {
  if (!closing) {
    return;
  }
  suppress_login_close_cleanup_ = true;
  CloseLoginWidgetForProcessExit();
}

void XenonLoginController::OnAppTerminatingClosingLogin() {
  suppress_login_close_cleanup_ = true;
  CloseLoginWidgetForProcessExit();
}

void XenonLoginController::CloseLoginWidgetForProcessExit() {
  if (!login_widget_ && !login_ui_open_) {
    return;
  }

  weak_factory_.InvalidateWeakPtrs();
  quit_after_login_widget_destroy_ = false;
  StopBrowserListObserving();
  StopAnchorFrameObservation();
  StopLoginWidgetObservation();
  ReleaseLoginGateKeepAlive();

  // Close before chrome::CloseAllBrowsers tears down browsers. On Windows,
  // Reparent off the frame, use ScopedAllowApplicationTasksInNativeNestedLoop
  // during DestroyWindow (TSF/IME nested delivery), then CloseNow — avoids
  // MessagePumpForUI DCHECKs with UIPumpImprovementsWin.
  if (login_widget_) {
#if BUILDFLAG(IS_WIN)
    base::CurrentThread::ScopedAllowApplicationTasksInNativeNestedLoop
        allow_nested_application_tasks;
    if (login_anchor_browser_ && login_anchor_browser_->window()) {
      if (BrowserView* browser_view =
              BrowserView::GetBrowserViewForBrowser(login_anchor_browser_)) {
        if (views::Widget* frame_widget = browser_view->GetWidget()) {
          if (login_widget_->parent() == frame_widget) {
            login_widget_->Reparent(nullptr);
          }
        }
      }
    }
    login_widget_->CloseNow();
#else
    login_widget_->Hide();
    login_widget_->Close();
#endif
  }

  login_widget_ = nullptr;
  login_anchor_browser_ = nullptr;
  login_ui_open_ = false;
}

void XenonLoginController::NotifyLoginDialogClosed() {
  if (!login_widget_ && !login_ui_open_) {
    return;
  }

  const bool skip_deferred_cleanup =
      suppress_login_close_cleanup_ ||
      browser_shutdown::HasShutdownStarted() ||
      browser_shutdown::IsTryingToQuit();
  suppress_login_close_cleanup_ = false;

  StopAnchorFrameObservation();
  login_anchor_browser_ = nullptr;

  const bool logged_in_at_close =
      login_profile_ && IsSessionLoggedIn(login_profile_);
  const bool had_deferred_launch = !pending_resume_launch_.is_null();

  // Logged-in close: ResumePendingLaunch() already moved the closure; do not
  // reset here. Dismiss-without-auth: cancel deferred browser launch.
  if (!logged_in_at_close) {
    pending_resume_launch_.Reset();
  }

  if (!skip_deferred_cleanup && !logged_in_at_close && had_deferred_launch) {
    // Quit runs from OnLoginWidgetDestroying once the Widget finishes teardown.
    // Keep observing until then — StopLoginWidgetObservation() here would skip
    // OnWidgetDestroying and leave the process pinned by keep-alives.
    quit_after_login_widget_destroy_ = true;
    login_ui_open_ = false;
    return;
  }

  StopLoginWidgetObservation();
  login_widget_ = nullptr;
  login_ui_open_ = false;
  StopBrowserListObserving();
  if (!logged_in_at_close) {
    RestoreHiddenBrowsers();
  }
}

void XenonLoginController::OnLoginWidgetDestroying(views::Widget* widget) {
  if (widget != login_widget_) {
    return;
  }

  const bool logged_in_at_close =
      login_profile_ && IsSessionLoggedIn(login_profile_);
  const bool should_quit =
      quit_after_login_widget_destroy_ ||
      (!logged_in_at_close && login_gate_keep_alive_);
  quit_after_login_widget_destroy_ = false;

  if (!logged_in_at_close) {
    pending_resume_launch_.Reset();
  }

  login_widget_ = nullptr;
  login_ui_open_ = false;
  StopBrowserListObserving();

  if (should_quit && !logged_in_at_close) {
    StopLoginWidgetObservation();
    QuitAfterLoginGateDismissed();
    return;
  }

  StopLoginWidgetObservation();

  if (!logged_in_at_close) {
    RestoreHiddenBrowsers();
  }
}

void XenonLoginController::QuitAfterLoginGateDismissed() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (browser_shutdown::HasShutdownStarted()) {
    return;
  }
  if (login_profile_ && IsSessionLoggedIn(login_profile_)) {
    return;
  }
  if (!login_gate_keep_alive_) {
    return;
  }

  ReleaseLoginGateKeepAlive();

  if (login_profile_ && g_browser_process &&
      g_browser_process->profile_manager()) {
    g_browser_process->profile_manager()->ClearFirstBrowserWindowKeepAlive(
        login_profile_);
  }

  if (g_browser_process) {
    if (BackgroundModeManager* background_mode =
            g_browser_process->background_mode_manager()) {
      // Component extension (service worker) may hold background-mode keep-
      // alives before the first Browser window is created.
      background_mode->SuspendBackgroundMode();
    }
  }

  XenonManager::GetInstance()->ShutdownForProcessExit();
  chrome::ExitIgnoreUnloadHandlers();
}

void XenonLoginController::AcquireLoginGateKeepAlive() {
  DCHECK(!login_gate_keep_alive_);
  login_gate_keep_alive_ = std::make_unique<ScopedKeepAlive>(
      KeepAliveOrigin::USER_MANAGER_VIEW, KeepAliveRestartOption::DISABLED);
}

void XenonLoginController::ReleaseLoginGateKeepAlive() {
  login_gate_keep_alive_.reset();
}

void XenonLoginController::ShowLoginDialog(Profile* profile, bool is_relogin) {
  if (!profile) {
    return;
  }
  suppress_login_close_cleanup_ = false;
  login_profile_ = profile->GetOriginalProfile();

  int presentation = 0;
  gfx::NativeView parent = gfx::NativeView();
  ui::mojom::ModalType modal_type = ui::mojom::ModalType::kNone;
  Browser* parent_browser = nullptr;
  if (is_relogin) {
    presentation = GetReloginPresentation(login_profile_);
    parent_browser = FindBrowserToParentLoginDialog(login_profile_);

    if (presentation == 1) {
      for (Browser* browser : *BrowserList::GetInstance()) {
        if (browser->window()) {
          browser->window()->Hide();
          hidden_browsers_.push_back(browser);
        }
      }
    }

    if (presentation == 2) {
      if (parent_browser && parent_browser->window()) {
        parent = parent_browser->window()->GetNativeWindow();
        modal_type = ui::mojom::ModalType::kWindow;
      }
    } else if (parent_browser && parent_browser->window()) {
      // presentation 0 / 1: non-modal, but still parent to the active session
      // browser so Z-order and ownership match the user's main window.
      parent = parent_browser->window()->GetNativeWindow();
    }
  }

  if (login_widget_) {
    ActivateLoginWidget();
    return;
  }

  raw_ptr<views::Widget> widget_out = nullptr;
  XenonWebDialog::ShowForLogin(
      login_profile_, LoginPageUrl(), 520, 640, u"登录", &widget_out, parent,
      modal_type,
      base::BindOnce(&XenonLoginController::NotifyLoginDialogClosed,
                     base::Unretained(GetInstance())));

  login_widget_ = widget_out;
  login_ui_open_ = true;
  if (login_widget_) {
    EnsureBrowserListObserving();
    StopLoginWidgetObservation();
    login_widget_observation_.Observe(login_widget_.get());
    Browser* anchor =
        is_relogin && parent_browser
            ? parent_browser
            : FindBrowserToParentLoginDialog(login_profile_);
    if (anchor) {
      ReparentLoginWidgetToBrowser(anchor);
    }
    login_widget_->Activate();
  }
}

void XenonLoginController::ResumePendingLaunch() {
  quit_after_login_widget_destroy_ = false;
  StopBrowserListObserving();
  RestoreHiddenBrowsers();
  base::OnceClosure task;
  if (pending_resume_launch_) {
    task = std::move(pending_resume_launch_);
  }
  if (login_widget_) {
    views::Widget* w = login_widget_;
    StopAnchorFrameObservation();
    StopLoginWidgetObservation();
    login_widget_ = nullptr;
    login_ui_open_ = false;
    login_anchor_browser_ = nullptr;
    w->Close();
  }
  // Launch the browser while login_gate_keep_alive_ is still held so
  // releasing it does not Unpin the process before the first Browser exists.
  if (task) {
    std::move(task).Run();
  }
  ReleaseLoginGateKeepAlive();
}

bool XenonLoginController::MaybeDeferLaunchBrowserForLastProfiles(
    StartupBrowserCreator* creator,
    const base::CommandLine& command_line,
    const base::FilePath& cur_dir,
    chrome::startup::IsProcessStartup process_startup,
    chrome::startup::IsFirstRun is_first_run,
    StartupProfileInfo profile_info,
    const StartupBrowserCreator::Profiles& last_opened_profiles,
    bool restore_tabbed_browser) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!creator || !IsLoginGateEnabled(command_line)) {
    return false;
  }
  if (profile_info.mode != StartupProfileMode::kBrowserWindow ||
      !profile_info.profile) {
    return false;
  }
  Profile* effective = profile_info.profile->GetOriginalProfile();
  if (IsSessionLoggedIn(effective)) {
    return false;
  }
  if (!pending_resume_launch_.is_null()) {
    ActivateLoginWidget();
    return true;
  }

  AcquireLoginGateKeepAlive();

  std::vector<GURL> first_run_urls = creator->first_run_tabs();
  StartupBrowserCreator::Profiles profiles_copy = last_opened_profiles;

  pending_resume_launch_ = base::BindOnce(
      [](std::vector<GURL> first_run_urls, base::CommandLine command_line,
         base::FilePath cur_dir,
         chrome::startup::IsProcessStartup process_startup,
         chrome::startup::IsFirstRun is_first_run,
         StartupProfileInfo profile_info,
         StartupBrowserCreator::Profiles last_opened_profiles,
         bool restore_tabbed_browser) {
        StartupBrowserCreator browser_creator;
        if (!first_run_urls.empty()) {
          browser_creator.AddFirstRunTabs(first_run_urls);
        }
        browser_creator.LaunchBrowserForLastProfiles(
            command_line, cur_dir, process_startup, is_first_run, profile_info,
            last_opened_profiles, restore_tabbed_browser);
      },
      std::move(first_run_urls), command_line, cur_dir, process_startup,
      is_first_run, profile_info, std::move(profiles_copy),
      restore_tabbed_browser);

  ShowLoginDialog(effective, /*is_relogin=*/false);
  return true;
}

bool XenonLoginController::MaybeDeferSingleBrowserLaunch(
    StartupBrowserCreator* creator,
    const base::CommandLine& command_line,
    Profile* profile,
    const base::FilePath& cur_dir,
    chrome::startup::IsProcessStartup process_startup,
    chrome::startup::IsFirstRun is_first_run,
    bool restore_tabbed_browser) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!creator || !profile || !IsLoginGateEnabled(command_line)) {
    return false;
  }
  Profile* effective = profile->GetOriginalProfile();
  if (IsSessionLoggedIn(effective)) {
    return false;
  }
  if (!pending_resume_launch_.is_null()) {
    ActivateLoginWidget();
    return true;
  }

  AcquireLoginGateKeepAlive();

  std::vector<GURL> first_run_urls = creator->first_run_tabs();

  pending_resume_launch_ = base::BindOnce(
      [](std::vector<GURL> first_run_urls, base::CommandLine command_line,
         Profile* profile, base::FilePath cur_dir,
         chrome::startup::IsProcessStartup process_startup,
         chrome::startup::IsFirstRun is_first_run, bool restore_tabbed_browser) {
        StartupBrowserCreator browser_creator;
        if (!first_run_urls.empty()) {
          browser_creator.AddFirstRunTabs(first_run_urls);
        }
        browser_creator.LaunchBrowser(command_line, profile, cur_dir,
                                      process_startup, is_first_run,
                                      restore_tabbed_browser);
      },
      std::move(first_run_urls), command_line, effective, cur_dir,
      process_startup, is_first_run, restore_tabbed_browser);

  ShowLoginDialog(effective, /*is_relogin=*/false);
  return true;
}

bool XenonLoginController::HandleSecondProcessDuringLogin() {
  if (!IsLoginGateEnabled(*base::CommandLine::ForCurrentProcess())) {
    return false;
  }
  if (pending_resume_launch_.is_null() && !login_ui_open_) {
    return false;
  }
  ActivateLoginWidget();
  return true;
}

void XenonLoginController::SetAppSessionLoggedIn(Profile* profile,
                                                 bool logged_in) {
  if (!profile) {
    return;
  }
  Profile* effective = profile->GetOriginalProfile();
  effective->GetPrefs()->SetBoolean(prefs::kAppSessionLoggedIn, logged_in);
  if (logged_in) {
    ResumePendingLaunch();
  } else {
    pending_resume_launch_.Reset();
    ShowLoginDialog(effective, /*is_relogin=*/true);
  }
}

}  // namespace xenon

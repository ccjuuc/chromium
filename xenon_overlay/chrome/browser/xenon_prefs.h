#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_PREFS_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_PREFS_H_

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace xenon::prefs {

// Session flag: main browser windows open only when true (after WebUI login).
inline constexpr char kAppSessionLoggedIn[] = "xenon.app_session_logged_in";

// 0 = standalone login window, 1 = hide all browser windows during relogin,
// 2 = modal dialog on last active browser window.
inline constexpr char kReloginPresentation[] = "xenon.relogin_presentation";

// One-shot: default-pin Xenon AI side panel action to the toolbar (Chromium
// side panels are opened via pinned toolbar actions; there is no separate icon
// until something is pinned).
inline constexpr char kAiSidePanelToolbarPinMigrated[] =
    "xenon.ai_side_panel_toolbar_pin_migrated";

// Whether the Xenon browser sidebar should auto-hide and appear from the active
// edge hot zone.
inline constexpr char kSidebarAutoHideEnabled[] =
    "xenon.sidebar_auto_hide_enabled";

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

}  // namespace xenon::prefs

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_PREFS_H_

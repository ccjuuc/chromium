// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// mini_installer is CRT-light and uses wchar_t*/argv style APIs by design.
#pragma allow_unsafe_buffers
#endif

#include "chrome/installer/mini_installer/mini_installer_ui.h"

#include <objbase.h>

#include <shobjidl.h>

#include <gdiplus.h>
#include <shellapi.h>
#include <stdio.h>

#include "UIlib.h"
#include "chrome/install_static/localized_product_name.h"
#include "chrome/installer/mini_installer/configuration.h"
#include "chrome/installer/mini_installer/mini_installer_resource.h"
#include "chrome/installer/mini_installer/regkey.h"

namespace mini_installer {
namespace {

constexpr UINT kMsgWorkDone = WM_USER + 100;
constexpr UINT kMsgSetProgress = WM_USER + 101;
constexpr UINT kTimerProgress = 1;

constexpr wchar_t kInstallBinaryDir[] = L"Application";
#if defined(CUSTOM_CHROME_PRODUCT_NAME_W)
constexpr wchar_t kFallbackProductName[] = CUSTOM_CHROME_PRODUCT_NAME_W;
#elif defined(CUSTOM_CHROME_PRODUCT_PATH_NAME_W)
constexpr wchar_t kFallbackProductName[] = CUSTOM_CHROME_PRODUCT_PATH_NAME_W;
#else
constexpr wchar_t kFallbackProductName[] = L"Chromium";
#endif
#if defined(CUSTOM_CHROME_PRODUCT_PATH_NAME_W)
constexpr wchar_t kProductPathName[] = CUSTOM_CHROME_PRODUCT_PATH_NAME_W;
#else
constexpr wchar_t kProductPathName[] = L"Chromium";
#endif
#if defined(CUSTOM_CHROME_EXE_NAME_W)
constexpr wchar_t kChromeExeName[] = CUSTOM_CHROME_EXE_NAME_W;
#else
constexpr wchar_t kChromeExeName[] = L"chrome.exe";
#endif

wchar_t g_brand_name[64] = {};
bool g_brand_name_ready = false;

DuiLib::CDuiString UiString(const wchar_t* id) {
  DuiLib::CDuiString text =
      DuiLib::CResourceManager::GetInstance()->GetText(id);
  // Use DuiLib's line-break convention for formatted strings and MessageBox
  // too.
  text.Replace(L"{\\n}", L"\n");
  return text;
}

// Matches installer::InstallStatus::EXISTING_VERSION_LAUNCHED. setup.exe
// returns this when a new user-level install conflicts with an existing
// system-level install (and historically auto-launched the old browser).
constexpr DWORD kExistingVersionLaunchedExitCode = 3;

// Includes 28px soft-shadow padding on each side baked into panel.png.
constexpr int kCollapsedClientHeight = 476;
constexpr int kExpandedClientHeight = 644;

bool BuildDefaultInstallPath(bool system_level,
                             wchar_t (&install_path)[MAX_PATH]) {
  const wchar_t* variable = system_level ? L"ProgramFiles" : L"LOCALAPPDATA";
  DWORD length = ::GetEnvironmentVariableW(variable, install_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return false;
  }

  const wchar_t* components[] = {L"\\", kProductPathName, L"\\",
                                 kInstallBinaryDir};
  for (const wchar_t* component : components) {
    const size_t component_length = ::lstrlenW(component);
    if (length + component_length >= MAX_PATH) {
      return false;
    }
    ::CopyMemory(install_path + length, component,
                 (component_length + 1) * sizeof(wchar_t));
    length += static_cast<DWORD>(component_length);
  }
  return true;
}

bool NormalizeInstallPath(const wchar_t* path,
                          wchar_t (&normalized)[MAX_PATH]) {
  if (!path || !*path) {
    return false;
  }
  const DWORD length = ::GetFullPathNameW(path, MAX_PATH, normalized, nullptr);
  if (length == 0 || length >= MAX_PATH || length <= 3 ||
      normalized[1] != L':' ||
      (normalized[2] != L'\\' && normalized[2] != L'/')) {
    return false;
  }

  wchar_t drive_root[] = {normalized[0], L':', L'\\', L'\0'};
  if (::GetDriveTypeW(drive_root) != DRIVE_FIXED) {
    return false;
  }

  size_t end = ::lstrlenW(normalized);
  while (end > 3 &&
         (normalized[end - 1] == L'\\' || normalized[end - 1] == L'/')) {
    normalized[--end] = L'\0';
  }

  wchar_t windows_path[MAX_PATH] = {};
  const UINT windows_length = ::GetWindowsDirectoryW(windows_path, MAX_PATH);
  if (windows_length == 0 || windows_length >= MAX_PATH) {
    return false;
  }
  const size_t normalized_windows_length = ::lstrlenW(windows_path);
  if (::_wcsnicmp(normalized, windows_path, normalized_windows_length) == 0 &&
      (normalized[normalized_windows_length] == L'\0' ||
       normalized[normalized_windows_length] == L'\\' ||
       normalized[normalized_windows_length] == L'/')) {
    return false;
  }
  return true;
}

#if defined(CUSTOM_CHROME_INSTALLATION_REG_KEY_W)
constexpr wchar_t kInstallationRegKey[] = CUSTOM_CHROME_INSTALLATION_REG_KEY_W;
#else
constexpr wchar_t kInstallationRegKey[] = L"Software\\Chromium";
#endif

// Converts "...\\Application\\<ver>\\Installer\\setup.exe" into
// "...\\Application". Returns false if the uninstall string is unusable.
bool InstallPathFromUninstallString(const wchar_t* uninstall,
                                    wchar_t (&install_path)[MAX_PATH]) {
  if (!uninstall || !*uninstall) {
    return false;
  }

  wchar_t setup_path[MAX_PATH] = {};
  const wchar_t* src = uninstall;
  if (*src == L'"') {
    ++src;
    size_t i = 0;
    while (*src && *src != L'"' && i + 1 < MAX_PATH) {
      setup_path[i++] = *src++;
    }
    setup_path[i] = L'\0';
  } else {
    size_t i = 0;
    while (*src && *src != L' ' && i + 1 < MAX_PATH) {
      setup_path[i++] = *src++;
    }
    setup_path[i] = L'\0';
  }

  wchar_t full[MAX_PATH] = {};
  if (::GetFullPathNameW(setup_path, MAX_PATH, full, nullptr) == 0 ||
      ::lstrlenW(full) <= 3) {
    return false;
  }

  // Strip setup.exe, Installer, <version>.
  for (int i = 0; i < 3; ++i) {
    wchar_t* slash = ::wcsrchr(full, L'\\');
    if (!slash) {
      slash = ::wcsrchr(full, L'/');
    }
    if (!slash || slash <= full + 2) {
      return false;
    }
    *slash = L'\0';
  }
  return NormalizeInstallPath(full, install_path);
}

bool FindInstallPathInRoot(HKEY root, wchar_t (&install_path)[MAX_PATH]) {
  wchar_t uninstall[MAX_PATH * 2] = {};
  return RegKey::ReadSZValue(root, kInstallationRegKey, L"UninstallString",
                             uninstall, _countof(uninstall)) &&
         InstallPathFromUninstallString(uninstall, install_path);
}

// Looks up an existing install for the requested scope only (no cross-hive
// fallback). Cross-scope conflicts are handled separately.
bool FindExistingInstallPath(bool system_level,
                             wchar_t (&install_path)[MAX_PATH]) {
  return FindInstallPathInRoot(
      system_level ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, install_path);
}

bool PathsEqualIgnoreCase(const wchar_t* a, const wchar_t* b) {
  return a && b && ::_wcsicmp(a, b) == 0;
}

// True when machine-level product exists but there is no user-level product.
// setup.exe refuses this combination for a new user-level install.
bool HasSystemLevelOnlyInstall() {
  wchar_t system_path[MAX_PATH] = {};
  wchar_t user_path[MAX_PATH] = {};
  return FindInstallPathInRoot(HKEY_LOCAL_MACHINE, system_path) &&
         !FindInstallPathInRoot(HKEY_CURRENT_USER, user_path);
}

void LaunchInstalledBrowser(const wchar_t* install_path) {
  if (!install_path || !*install_path) {
    return;
  }

  wchar_t chrome_exe[MAX_PATH] = {};
  const size_t path_len = ::lstrlenW(install_path);
  const size_t exe_len = ::lstrlenW(kChromeExeName);
  const bool need_slash =
      install_path[path_len - 1] != L'\\' && install_path[path_len - 1] != L'/';
  if (path_len + (need_slash ? 1 : 0) + exe_len >= MAX_PATH) {
    return;
  }

  ::lstrcpynW(chrome_exe, install_path, MAX_PATH);
  if (need_slash) {
    ::lstrcatW(chrome_exe, L"\\");
  }
  ::lstrcatW(chrome_exe, kChromeExeName);

  if (::GetFileAttributesW(chrome_exe) == INVALID_FILE_ATTRIBUTES) {
    return;
  }

  ::ShellExecuteW(nullptr, L"open", chrome_exe, nullptr, nullptr,
                  SW_SHOWNORMAL);
}

enum class InstallerMsgButtons {
  kOk,
  kYesNo,
};

class MsgDlg : public DuiLib::WindowImplBase {
 public:
  MsgDlg(const wchar_t* title,
         const wchar_t* message,
         InstallerMsgButtons buttons)
      : title_(title ? DuiLib::CDuiString(title)
                     : UiString(L"IDS_MINI_NOTICE")),
        message_(message ? message : L""),
        buttons_(buttons) {}

  DuiLib::CDuiString GetSkinFile() override {
    return DuiLib::CDuiString(_T("DlgMsg.xml"));
  }

  LPCTSTR GetWindowClassName() const override {
    return _T("ChromeMiniInstallerMsg");
  }

  void InitWindow() override {
    auto* title =
        static_cast<DuiLib::CLabelUI*>(m_pm.FindControl(_T("msgtitle")));
    auto* body =
        static_cast<DuiLib::CLabelUI*>(m_pm.FindControl(_T("msgbody")));
    auto* yes_btn =
        static_cast<DuiLib::CButtonUI*>(m_pm.FindControl(_T("msgyes")));
    auto* no_btn =
        static_cast<DuiLib::CButtonUI*>(m_pm.FindControl(_T("msgno")));
    auto* ok_btn =
        static_cast<DuiLib::CButtonUI*>(m_pm.FindControl(_T("msgok")));
    auto* space = m_pm.FindControl(_T("msgbtnspace"));

    if (title) {
      title->SetText(title_);
    }
    if (body) {
      body->SetText(message_);
    }

    const bool yes_no = buttons_ == InstallerMsgButtons::kYesNo;
    if (yes_btn) {
      yes_btn->SetVisible(yes_no);
    }
    if (no_btn) {
      no_btn->SetVisible(yes_no);
    }
    if (space) {
      space->SetVisible(yes_no);
    }
    if (ok_btn) {
      ok_btn->SetVisible(!yes_no);
    }
  }

  void OnFinalMessage(HWND hwnd) override {
    DuiLib::WindowImplBase::OnFinalMessage(hwnd);
    delete this;
  }

  // Avoid CreateRoundRectRgn jagged clips; panel.png provides AA corners +
  // soft shadow.
  LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL& handled) override {
    if (GetHWND() && !::IsIconic(GetHWND())) {
      ::SetWindowRgn(GetHWND(), nullptr, TRUE);
    }
    handled = FALSE;
    return 0;
  }

  void Notify(DuiLib::TNotifyUI& msg) override {
    if (msg.sType == _T("click") && msg.pSender) {
      const DuiLib::CDuiString name = msg.pSender->GetName();
      if (name == _T("msgyes")) {
        Close(IDYES);
        return;
      }
      if (name == _T("msgno") || name == _T("msgclose")) {
        Close(buttons_ == InstallerMsgButtons::kYesNo ? IDNO : IDOK);
        return;
      }
      if (name == _T("msgok")) {
        Close(IDOK);
        return;
      }
    }
    DuiLib::WindowImplBase::Notify(msg);
  }

 private:
  DuiLib::CDuiString title_;
  DuiLib::CDuiString message_;
  InstallerMsgButtons buttons_;
};

// Skin-matching modal prompt. Falls back to MessageBox if create fails.
int ShowInstallerMessage(HWND owner,
                         const wchar_t* title,
                         const wchar_t* message,
                         InstallerMsgButtons buttons) {
  auto* dlg = new MsgDlg(title, message, buttons);
  dlg->Create(owner,
              title ? DuiLib::CDuiString(title) : UiString(L"IDS_MINI_NOTICE"),
              WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0);
  if (!dlg->GetHWND()) {
    delete dlg;
    const UINT type = buttons == InstallerMsgButtons::kYesNo
                          ? (MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2)
                          : (MB_OK | MB_ICONWARNING);
    return ::MessageBoxW(owner, message, title, type);
  }
  dlg->CenterWindow();
  return static_cast<int>(dlg->ShowModal());
}

const wchar_t* ProductName() {
  return g_brand_name[0]
             ? g_brand_name
             : install_static::LocalizedProductName(kFallbackProductName);
}

void FormatProductTitle(wchar_t (&title)[64]) {
  ::_snwprintf_s(title, _TRUNCATE, UiString(L"IDS_MINI_INSTALLED"),
                 ProductName());
}

// Returns false if the user cancelled / must fix the path before installing.
bool ConfirmExistingInstall(HWND owner,
                            bool system_level,
                            wchar_t (&selected_path)[MAX_PATH]) {
  wchar_t title[64] = {};
  FormatProductTitle(title);

  if (!system_level && HasSystemLevelOnlyInstall()) {
    wchar_t body[256] = {};
    ::_snwprintf_s(body, _TRUNCATE, UiString(L"IDS_MINI_SYSTEM_CONFLICT"),
                   ProductName());
    ShowInstallerMessage(owner, title, body, InstallerMsgButtons::kOk);
    return false;
  }

  wchar_t existing[MAX_PATH] = {};
  if (!FindExistingInstallPath(system_level, existing)) {
    return true;
  }

  if (PathsEqualIgnoreCase(existing, selected_path)) {
    wchar_t body[320] = {};
    ::_snwprintf_s(body, _TRUNCATE, UiString(L"IDS_MINI_UPGRADE_CONFIRM"),
                   ProductName());
    return ShowInstallerMessage(owner, title, body,
                                InstallerMsgButtons::kYesNo) == IDYES;
  }

  wchar_t message[MAX_PATH * 2 + 180] = {};
  ::_snwprintf_s(message, _TRUNCATE, UiString(L"IDS_MINI_PATH_CONFLICT"),
                 ProductName(), existing);
  ShowInstallerMessage(owner, title, message, InstallerMsgButtons::kOk);

  // Snap the UI path back to the existing install location.
  ::lstrcpynW(selected_path, existing, MAX_PATH);
  return false;
}

bool SelectInstallFolder(HWND owner, wchar_t (&selected_path)[MAX_PATH]) {
  IFileDialog* dialog = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (FAILED(hr)) {
    return false;
  }

  DWORD options = 0;
  hr = dialog->GetOptions(&options);
  if (SUCCEEDED(hr)) {
    hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                            FOS_PATHMUSTEXIST);
  }
  if (SUCCEEDED(hr)) {
    hr = dialog->Show(owner);
  }

  IShellItem* item = nullptr;
  if (SUCCEEDED(hr)) {
    hr = dialog->GetResult(&item);
  }

  PWSTR path = nullptr;
  if (SUCCEEDED(hr)) {
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
  }
  if (SUCCEEDED(hr)) {
    ::lstrcpynW(selected_path, path, MAX_PATH);
  }

  if (path) {
    ::CoTaskMemFree(path);
  }
  if (item) {
    item->Release();
  }
  dialog->Release();
  return SUCCEEDED(hr);
}

bool CommandLineHasSilentSwitch(const Configuration& configuration) {
  int argc = 0;
  wchar_t** argv = ::CommandLineToArgvW(configuration.command_line(), &argc);
  if (!argv) {
    return false;
  }
  bool silent = false;
  for (int i = 1; i < argc; ++i) {
    if (::_wcsicmp(argv[i], L"--silent") == 0 ||
        ::_wcsicmp(argv[i], L"--quiet") == 0 ||
        ::_wcsicmp(argv[i], L"/S") == 0 || ::_wcsicmp(argv[i], L"/s") == 0) {
      silent = true;
      break;
    }
  }
  ::LocalFree(argv);
  return silent;
}

bool InitSkinFromZipResource(HMODULE module) {
  DuiLib::CPaintManagerUI::SetInstance(module);
  DuiLib::CPaintManagerUI::SetResourceDll(module);
  DuiLib::CPaintManagerUI::SetResourceType(DuiLib::UILIB_ZIPRESOURCE);

  HRSRC resource =
      ::FindResourceW(module, MAKEINTRESOURCEW(IDR_DUILIB_SKIN), L"ZIPRES");
  if (!resource) {
    return false;
  }
  HGLOBAL global = ::LoadResource(module, resource);
  if (!global) {
    return false;
  }
  const DWORD size = ::SizeofResource(module, resource);
  void* data = ::LockResource(global);
  if (!data || size == 0) {
    return false;
  }

  DuiLib::CPaintManagerUI::SetResourceZip(data, size);
  auto* resources = DuiLib::CResourceManager::GetInstance();
  // English is the fallback for missing translations and non-Chinese locales.
  if (!resources->LoadLanguage(L"lang/en-US.xml")) {
    return false;
  }
  if (install_static::UseChineseProductName() &&
      !resources->LoadLanguage(L"lang/zh-CN.xml")) {
    return false;
  }
  return true;
}

struct WorkContext {
  ProcessExitResult (*work)(void*, HWND);
  void* ctx;
  ProcessExitResult result;
  HWND hwnd;
  HANDLE thread;
};

DWORD WINAPI WorkThreadProc(void* param) {
  auto* work_ctx = static_cast<WorkContext*>(param);
  work_ctx->result = work_ctx->work(work_ctx->ctx, work_ctx->hwnd);
  if (work_ctx->hwnd) {
    ::PostMessageW(work_ctx->hwnd, kMsgWorkDone, 0, 0);
  }
  return 0;
}

DuiLib::CDuiString StageText(InstallerUiStage stage) {
  switch (stage) {
    case kInstallerUiStageExtractArchive:
      return UiString(L"IDS_MINI_EXTRACT_ARCHIVE");
    case kInstallerUiStageExtractSetup:
      return UiString(L"IDS_MINI_EXTRACT_SETUP");
    case kInstallerUiStageInstall: {
      static wchar_t installing[96] = {};
      if (!installing[0]) {
        ::_snwprintf_s(installing, _TRUNCATE, UiString(L"IDS_MINI_INSTALLING"),
                       ProductName());
      }
      return installing;
    }
    case kInstallerUiStageFinishing:
      return UiString(L"IDS_MINI_FINISHING");
    case kInstallerUiStageFailed:
      return UiString(L"IDS_MINI_FAILED");
    default:
      return L"";
  }
}

DuiLib::CDuiString FailureText(DWORD exit_code) {
  if (exit_code == kExistingVersionLaunchedExitCode) {
    return UiString(L"IDS_MINI_SYSTEM_EXISTS");
  }
  return StageText(kInstallerUiStageFailed);
}

class SplashWnd : public DuiLib::WindowImplBase {
 public:
  SplashWnd(WorkContext* work_ctx,
            const Configuration& configuration,
            InstallerUiOptions* ui_options)
      : work_ctx_(work_ctx),
        configuration_(configuration),
        ui_options_(ui_options) {}

  DuiLib::CDuiString GetSkinFile() override {
    return DuiLib::CDuiString(_T("DlgMain.xml"));
  }

  LPCTSTR GetWindowClassName() const override {
    return _T("ChromeMiniInstaller");
  }

  void InitWindow() override {
    progress_ =
        static_cast<DuiLib::CProgressUI*>(m_pm.FindControl(_T("install")));
    text_ =
        static_cast<DuiLib::CLabelUI*>(m_pm.FindControl(_T("textProgress")));
    install_button_ =
        static_cast<DuiLib::CButtonUI*>(m_pm.FindControl(_T("installbtn")));
    custom_button_ =
        static_cast<DuiLib::CButtonUI*>(m_pm.FindControl(_T("custombtn")));
    custom_panel_ = m_pm.FindControl(_T("custompanel"));
    install_path_edit_ =
        static_cast<DuiLib::CEditUI*>(m_pm.FindControl(_T("installpath")));
    privacy_option_ =
        static_cast<DuiLib::COptionUI*>(m_pm.FindControl(_T("privacy")));
    system_option_ =
        static_cast<DuiLib::COptionUI*>(m_pm.FindControl(_T("systemlevel")));
    desktop_option_ =
        static_cast<DuiLib::COptionUI*>(m_pm.FindControl(_T("desktop")));
    launch_option_ =
        static_cast<DuiLib::COptionUI*>(m_pm.FindControl(_T("launch")));
    default_browser_option_ =
        static_cast<DuiLib::COptionUI*>(m_pm.FindControl(_T("defaultbrowser")));

    BuildDefaultInstallPath(false, user_install_path_);
    BuildDefaultInstallPath(true, system_install_path_);

    if (auto* brand_title = static_cast<DuiLib::CLabelUI*>(
            m_pm.FindControl(_T("brandtitle")))) {
      brand_title->SetText(ProductName());
    }
    if (auto* brand_hero =
            static_cast<DuiLib::CLabelUI*>(m_pm.FindControl(_T("brandhero")))) {
      brand_hero->SetText(ProductName());
    }

    if (system_option_) {
      system_option_->Selected(configuration_.is_system_level());
      system_option_->SetEnabled(!configuration_.is_system_level());
    }
    if (desktop_option_) {
      desktop_option_->Selected(true);
    }
    if (launch_option_) {
      launch_option_->Selected(true);
    }
    if (privacy_option_) {
      privacy_option_->Selected(false);
    }
    bool has_existing_for_scope = false;
    if (install_path_edit_) {
      wchar_t existing[MAX_PATH] = {};
      const bool system_level = configuration_.is_system_level();
      if (FindExistingInstallPath(system_level, existing)) {
        install_path_edit_->SetText(existing);
        has_existing_for_scope = true;
      } else {
        install_path_edit_->SetText(system_level ? system_install_path_
                                                 : user_install_path_);
      }
    }
    if (custom_panel_) {
      custom_panel_->SetVisible(false);
    }
    if (progress_) {
      progress_->SetVisible(false);
    }
    if (text_) {
      text_->SetText(has_existing_for_scope ? UiString(L"IDS_MINI_UPGRADE")
                                            : DuiLib::CDuiString());
    }
    UpdateInstallEnabled();
    SetCustomExpanded(false);
    if (!install_button_) {
      StartWork();
    }
  }

  void OnFinalMessage(HWND hwnd) override {
    ::KillTimer(hwnd, kTimerProgress);
    DuiLib::WindowImplBase::OnFinalMessage(hwnd);
    ::PostQuitMessage(0);
  }

  // Avoid CreateRoundRectRgn jagged clips; panel.png provides AA corners +
  // soft shadow.
  LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL& handled) override {
    if (GetHWND() && !::IsIconic(GetHWND())) {
      ::SetWindowRgn(GetHWND(), nullptr, TRUE);
    }
    handled = FALSE;
    return 0;
  }

  LRESULT HandleCustomMessage(UINT msg,
                              WPARAM wparam,
                              LPARAM lparam,
                              BOOL& handled) override {
    if (msg == WM_TIMER && wparam == kTimerProgress) {
      if (progress_ && !work_done_) {
        int value = progress_->GetValue();
        // Creep slowly during long setup.exe work without overshooting.
        const int cap = progress_cap_ > 0 ? progress_cap_ : 90;
        if (value < cap) {
          progress_->SetValue(value + 1);
        }
      }
      handled = TRUE;
      return 0;
    }
    if (msg == kMsgSetProgress) {
      const int percent = static_cast<int>(wparam);
      const auto stage = static_cast<InstallerUiStage>(lparam);
      if (progress_) {
        progress_->SetVisible(true);
        if (percent > progress_->GetValue()) {
          progress_->SetValue(percent);
        }
      }
      if (stage == kInstallerUiStageExtractArchive) {
        progress_cap_ = 28;
      } else if (stage == kInstallerUiStageExtractSetup) {
        progress_cap_ = 42;
      } else if (stage == kInstallerUiStageInstall) {
        progress_cap_ = 92;
      } else if (stage == kInstallerUiStageFinishing) {
        progress_cap_ = 99;
      }
      if (text_) {
        text_->SetText(StageText(stage));
      }
      handled = TRUE;
      return 0;
    }
    if (msg == kMsgWorkDone) {
      ShowFinishedState();
      handled = TRUE;
      return 0;
    }
    return 0;
  }

  void Notify(DuiLib::TNotifyUI& msg) override {
    if (msg.sType == _T("click") || msg.sType == _T("selectchanged")) {
      if (msg.pSender && msg.pSender->GetName() == _T("installbtn")) {
        if (finish_mode_) {
          // Launch only on an explicit Finish click when the option is set.
          if (work_ctx_ && work_ctx_->result.IsSuccess() && ui_options_ &&
              ui_options_->launch_after_install && !launched_after_install_) {
            LaunchInstalledBrowser(ui_options_->install_path);
            launched_after_install_ = true;
          }
          ::PostMessageW(GetHWND(), WM_CLOSE, 0, 0);
          return;
        }
        StartWork();
        return;
      }
      if (msg.pSender && msg.pSender->GetName() == _T("custombtn")) {
        if (!work_started_) {
          SetCustomExpanded(!custom_expanded_);
        }
        return;
      }
      if (msg.pSender && msg.pSender->GetName() == _T("browsebtn")) {
        if (work_started_) {
          return;
        }
        wchar_t selected_path[MAX_PATH] = {};
        if (SelectInstallFolder(GetHWND(), selected_path) &&
            install_path_edit_) {
          install_path_edit_->SetText(selected_path);
        }
        return;
      }
      if (msg.pSender && msg.pSender->GetName() == _T("privacy")) {
        UpdateInstallEnabled();
        return;
      }
      if (msg.pSender && msg.pSender->GetName() == _T("systemlevel")) {
        UpdateDefaultInstallPath();
        return;
      }
      if (msg.pSender && msg.pSender->GetName() == _T("closebtn")) {
        // Block closing while unpack/setup is running.
        if (work_started_ && !finish_mode_) {
          return;
        }
        ::PostMessageW(GetHWND(), WM_CLOSE, 0, 0);
        return;
      }
    }
    DuiLib::WindowImplBase::Notify(msg);
  }

 private:
  void UpdateInstallEnabled() {
    if (!install_button_ || work_started_) {
      return;
    }
    install_button_->SetEnabled(!privacy_option_ ||
                                privacy_option_->IsSelected());
  }

  void SetCustomExpanded(bool expanded) {
    custom_expanded_ = expanded;
    if (custom_panel_) {
      custom_panel_->SetVisible(expanded);
    }
    if (custom_button_) {
      custom_button_->SetText(expanded ? UiString(L"IDS_MINI_CUSTOM_EXPANDED")
                                       : UiString(L"IDS_MINI_CUSTOM"));
    }

    HWND hwnd = GetHWND();
    if (!hwnd) {
      return;
    }

    RECT window_rect = {};
    ::GetWindowRect(hwnd, &window_rect);
    RECT client_rect = {};
    ::GetClientRect(hwnd, &client_rect);
    const int frame_width =
        (window_rect.right - window_rect.left) - client_rect.right;
    const int frame_height =
        (window_rect.bottom - window_rect.top) - client_rect.bottom;
    const int client_height =
        expanded ? kExpandedClientHeight : kCollapsedClientHeight;
    ::SetWindowPos(hwnd, nullptr, window_rect.left, window_rect.top,
                   client_rect.right + frame_width,
                   client_height + frame_height, SWP_NOZORDER | SWP_NOACTIVATE);
    CenterWindow();
    if (m_pm.GetRoot()) {
      m_pm.NeedUpdate();
    }
  }

  void UpdateDefaultInstallPath() {
    if (!install_path_edit_ || !system_option_) {
      return;
    }
    const bool system_level = system_option_->IsSelected();
    const DuiLib::CDuiString current = install_path_edit_->GetText();
    if (::_wcsicmp(current.GetData(), user_install_path_) != 0 &&
        ::_wcsicmp(current.GetData(), system_install_path_) != 0) {
      return;
    }
    wchar_t existing[MAX_PATH] = {};
    if (FindExistingInstallPath(system_level, existing)) {
      install_path_edit_->SetText(existing);
      if (text_) {
        text_->SetText(UiString(L"IDS_MINI_UPGRADE"));
      }
    } else {
      install_path_edit_->SetText(system_level ? system_install_path_
                                               : user_install_path_);
      if (text_) {
        text_->SetText(_T(""));
      }
    }
  }

  bool SaveOptions() {
    if (!ui_options_ || !install_path_edit_ ||
        (privacy_option_ && !privacy_option_->IsSelected())) {
      if (text_) {
        text_->SetText(UiString(L"IDS_MINI_ACCEPT_PRIVACY"));
      }
      return false;
    }

    const DuiLib::CDuiString path = install_path_edit_->GetText();
    wchar_t normalized[MAX_PATH] = {};
    if (!NormalizeInstallPath(path.GetData(), normalized)) {
      if (text_) {
        text_->SetText(UiString(L"IDS_MINI_INVALID_PATH"));
      }
      return false;
    }

    const bool system_level = system_option_ && system_option_->IsSelected();
    if (!ConfirmExistingInstall(GetHWND(), system_level, normalized)) {
      if (install_path_edit_) {
        install_path_edit_->SetText(normalized);
      }
      if (text_) {
        text_->SetText(UiString(L"IDS_MINI_CONFIRM_PATH"));
      }
      return false;
    }

    ::lstrcpynW(ui_options_->install_path, normalized, MAX_PATH);
    ui_options_->apply_options = true;
    ui_options_->privacy_accepted =
        !privacy_option_ || privacy_option_->IsSelected();
    ui_options_->system_level = system_level;
    ui_options_->create_desktop_shortcut =
        !desktop_option_ || desktop_option_->IsSelected();
    ui_options_->launch_after_install =
        !launch_option_ || launch_option_->IsSelected();
    ui_options_->make_default_browser =
        default_browser_option_ && default_browser_option_->IsSelected();
    return true;
  }

  void ShowFinishedState() {
    work_done_ = true;
    finish_mode_ = true;
    ::KillTimer(GetHWND(), kTimerProgress);

    const bool success = work_ctx_ && work_ctx_->result.IsSuccess();
    if (progress_) {
      progress_->SetVisible(true);
      progress_->SetValue(success ? 100 : progress_->GetValue());
    }
    if (text_) {
      if (success) {
        text_->SetText(UiString(L"IDS_MINI_COMPLETE"));
      } else {
        text_->SetText(FailureText(work_ctx_ ? work_ctx_->result.exit_code
                                             : GENERIC_ERROR));
      }
    }
    if (privacy_option_) {
      privacy_option_->SetVisible(false);
    }
    if (custom_button_) {
      custom_button_->SetVisible(false);
    }
    if (custom_expanded_) {
      SetCustomExpanded(false);
    }

    if (install_button_) {
      install_button_->SetText(success ? UiString(L"IDS_MINI_FINISH")
                                       : UiString(L"IDS_MINI_CLOSE"));
      install_button_->SetVisible(true);
      install_button_->SetEnabled(true);
    }
  }

  void StartWork() {
    if (work_started_ || !work_ctx_ || !SaveOptions()) {
      return;
    }
    work_started_ = true;

    if (install_button_) {
      install_button_->SetEnabled(false);
      install_button_->SetVisible(false);
    }
    if (privacy_option_) {
      privacy_option_->SetVisible(false);
    }
    if (custom_button_) {
      custom_button_->SetVisible(false);
    }
    if (custom_expanded_) {
      SetCustomExpanded(false);
    }
    if (progress_) {
      progress_->SetVisible(true);
      progress_->SetValue(5);
    }
    progress_cap_ = 28;
    if (text_) {
      text_->SetText(StageText(kInstallerUiStageExtractArchive));
    }

    ::SetTimer(GetHWND(), kTimerProgress, 180, nullptr);
    work_ctx_->hwnd = GetHWND();
    work_ctx_->thread =
        ::CreateThread(nullptr, 0, WorkThreadProc, work_ctx_, 0, nullptr);
    if (!work_ctx_->thread) {
      work_ctx_->result =
          ProcessExitResult(GENERIC_INITIALIZATION_FAILURE, ::GetLastError());
      ::PostMessageW(GetHWND(), kMsgWorkDone, 0, 0);
    }
  }

  WorkContext* work_ctx_ = nullptr;
  const Configuration& configuration_;
  InstallerUiOptions* ui_options_ = nullptr;
  DuiLib::CProgressUI* progress_ = nullptr;
  DuiLib::CLabelUI* text_ = nullptr;
  DuiLib::CButtonUI* install_button_ = nullptr;
  DuiLib::CButtonUI* custom_button_ = nullptr;
  DuiLib::CControlUI* custom_panel_ = nullptr;
  DuiLib::CEditUI* install_path_edit_ = nullptr;
  DuiLib::COptionUI* privacy_option_ = nullptr;
  DuiLib::COptionUI* system_option_ = nullptr;
  DuiLib::COptionUI* desktop_option_ = nullptr;
  DuiLib::COptionUI* launch_option_ = nullptr;
  DuiLib::COptionUI* default_browser_option_ = nullptr;
  wchar_t user_install_path_[MAX_PATH] = {};
  wchar_t system_install_path_[MAX_PATH] = {};
  bool work_started_ = false;
  bool work_done_ = false;
  bool finish_mode_ = false;
  bool custom_expanded_ = false;
  bool launched_after_install_ = false;
  int progress_cap_ = 90;
};

}  // namespace

void InitInstallerBrandName(HMODULE, const Configuration&) {
  if (g_brand_name_ready) {
    return;
  }

  ::lstrcpynW(g_brand_name,
              install_static::LocalizedProductName(kFallbackProductName),
              _countof(g_brand_name));
  g_brand_name_ready = true;
}

const wchar_t* InstallerBrandName() {
  return g_brand_name[0]
             ? g_brand_name
             : install_static::LocalizedProductName(kFallbackProductName);
}

bool ShouldShowInstallerUi(const Configuration& configuration) {
  return !CommandLineHasSilentSwitch(configuration);
}

bool PrepareSilentInstallerOptions(const Configuration& configuration,
                                   InstallerUiOptions* options) {
  if (!options) {
    return false;
  }

  wchar_t user_path[MAX_PATH] = {};
  wchar_t system_path[MAX_PATH] = {};
  const bool has_user = FindInstallPathInRoot(HKEY_CURRENT_USER, user_path);
  const bool has_system =
      FindInstallPathInRoot(HKEY_LOCAL_MACHINE, system_path);

  // Honor an explicit --system-level; never silently downgrade it.
  const bool want_system = configuration.is_system_level();
  bool system_level = want_system;
  wchar_t chosen[MAX_PATH] = {};

  if (want_system) {
    if (has_system) {
      ::lstrcpynW(chosen, system_path, MAX_PATH);
    } else if (!BuildDefaultInstallPath(true, chosen)) {
      return false;
    }
  } else if (has_user) {
    ::lstrcpynW(chosen, user_path, MAX_PATH);
  } else if (has_system) {
    // No --system-level, but a machine install exists: upgrade in place.
    ::lstrcpynW(chosen, system_path, MAX_PATH);
    system_level = true;
  } else if (!BuildDefaultInstallPath(false, chosen)) {
    return false;
  }

  wchar_t normalized[MAX_PATH] = {};
  if (!NormalizeInstallPath(chosen, normalized)) {
    return false;
  }

  ::lstrcpynW(options->install_path, normalized, MAX_PATH);
  options->apply_options = true;
  options->system_level = system_level;
  options->create_desktop_shortcut = true;
  options->launch_after_install = false;
  options->make_default_browser = false;
  options->privacy_accepted = true;
  return true;
}

void PostInstallerUiProgress(HWND hwnd, int percent, InstallerUiStage stage) {
  if (!hwnd) {
    return;
  }
  ::PostMessageW(hwnd, kMsgSetProgress, static_cast<WPARAM>(percent),
                 static_cast<LPARAM>(stage));
}

ProcessExitResult RunInstallerWorkWithoutUi(
    const Configuration& configuration,
    InstallerUiOptions* options,
    ProcessExitResult (*work)(void* ctx, HWND progress_hwnd),
    void* ctx) {
  // Keep the same safety guarantees as silent mode (path reuse + no auto
  // launch) when the interactive skin fails to load.
  if (!PrepareSilentInstallerOptions(configuration, options)) {
    return ProcessExitResult(GENERIC_INITIALIZATION_FAILURE);
  }
  return work(ctx, nullptr);
}

ProcessExitResult RunWithInstallerUi(
    HMODULE module,
    const Configuration& configuration,
    InstallerUiOptions* options,
    ProcessExitResult (*work)(void* ctx, HWND progress_hwnd),
    void* ctx) {
  if (!work || !options) {
    return ProcessExitResult(GENERIC_INITIALIZATION_FAILURE);
  }

  InitInstallerBrandName(module, configuration);

  HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool com_inited = SUCCEEDED(com_hr) || com_hr == RPC_E_CHANGED_MODE;

  Gdiplus::GdiplusStartupInput gdiplus_input;
  ULONG_PTR gdiplus_token = 0;
  const Gdiplus::Status gdi_status =
      Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr);

  if (!InitSkinFromZipResource(module) || gdi_status != Gdiplus::Ok) {
    if (gdi_status == Gdiplus::Ok) {
      Gdiplus::GdiplusShutdown(gdiplus_token);
    }
    if (com_inited && SUCCEEDED(com_hr)) {
      ::CoUninitialize();
    }
    return RunInstallerWorkWithoutUi(configuration, options, work, ctx);
  }

  WorkContext work_ctx = {work, ctx, ProcessExitResult(INSTALLER_CANCELLED),
                          nullptr, nullptr};

  SplashWnd* frame = new SplashWnd(&work_ctx, configuration, options);
  // Layered + anti-aliased panel.png provides smooth corners; avoid
  // WS_EX_WINDOWEDGE which draws a hard non-AA frame around the window.
  wchar_t window_title[64] = {};
  ::_snwprintf_s(window_title, _TRUNCATE, UiString(L"IDS_MINI_WINDOW_TITLE"),
                 InstallerBrandName());
  frame->Create(nullptr, window_title, UI_WNDSTYLE_FRAME, WS_EX_APPWINDOW);
  if (!frame->GetHWND()) {
    delete frame;
    Gdiplus::GdiplusShutdown(gdiplus_token);
    if (com_inited && SUCCEEDED(com_hr)) {
      ::CoUninitialize();
    }
    return RunInstallerWorkWithoutUi(configuration, options, work, ctx);
  }
  frame->CenterWindow();
  frame->ShowWindow(true);

  DuiLib::CPaintManagerUI::MessageLoop();
  if (work_ctx.thread) {
    ::WaitForSingleObject(work_ctx.thread, INFINITE);
    ::CloseHandle(work_ctx.thread);
  }

  // WindowImplBase::OnFinalMessage does not delete `this` by default unless
  // subclass does; SplashWnd leaves lifetime to us after MessageLoop.
  delete frame;

  Gdiplus::GdiplusShutdown(gdiplus_token);
  if (com_inited && SUCCEEDED(com_hr)) {
    ::CoUninitialize();
  }

  return work_ctx.result;
}

}  // namespace mini_installer

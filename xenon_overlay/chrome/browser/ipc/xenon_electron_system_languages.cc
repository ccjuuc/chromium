// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_system_languages.h"

#include <utility>

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.system.userprofile.h>
#include <wrl.h>

#include <optional>

#include "base/strings/sys_string_conversions.h"
#include "base/win/core_winrt_util.h"
#include "base/win/i18n.h"
#elif BUILDFLAG(IS_MAC)
#include <CoreFoundation/CoreFoundation.h>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/strings/sys_string_conversions.h"
#elif BUILDFLAG(IS_LINUX) && defined(USE_GLIB)
#include <glib.h>

#include <string_view>

#include "base/compiler_specific.h"
#include "base/i18n/rtl.h"
#endif

namespace xenon::ipc {

#if BUILDFLAG(IS_WIN)
namespace {

std::optional<std::vector<std::string>> ReadWindowsLanguagePreferences() {
  const auto class_name = base::win::ScopedHString::Create(
      RuntimeClass_Windows_System_UserProfile_GlobalizationPreferences);
  if (!class_name.is_valid()) {
    return std::nullopt;
  }
  Microsoft::WRL::ComPtr<
      ABI::Windows::System::UserProfile::IGlobalizationPreferencesStatics>
      preferences;
  if (FAILED(base::win::RoGetActivationFactory(class_name.get(),
                                               IID_PPV_ARGS(&preferences)))) {
    return std::nullopt;
  }
  Microsoft::WRL::ComPtr<
      ABI::Windows::Foundation::Collections::IVectorView<HSTRING>>
      languages;
  if (FAILED(preferences->get_Languages(&languages))) {
    return std::nullopt;
  }
  unsigned count = 0;
  if (FAILED(languages->get_Size(&count))) {
    return std::nullopt;
  }
  std::vector<std::string> result;
  result.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    base::win::ScopedHString language(nullptr);
    if (FAILED(languages->GetAt(
            i, base::win::ScopedHString::Receiver(language).get()))) {
      // Use the fallback for the entire snapshot, not a truncated list.
      return std::nullopt;
    }
    result.push_back(language.GetAsUTF8());
  }
  return result;
}

}  // namespace
#endif

base::expected<std::vector<std::string>, std::string>
GetPreferredSystemLanguages() {
#if BUILDFLAG(IS_WIN)
  // This API includes preferred languages without an installed UI pack. The
  // browser owns COM/WinRT initialization; do not change the caller's
  // apartment.
  if (auto languages = ReadWindowsLanguagePreferences()) {
    return std::move(*languages);
  }
  std::vector<std::wstring> languages;
  if (!base::win::i18n::GetThreadPreferredUILanguageList(&languages)) {
    return base::unexpected(
        "ERR_SYSTEM_ERROR: Windows language preferences are unavailable");
  }
  std::vector<std::string> result;
  result.reserve(languages.size());
  for (const auto& language : languages) {
    result.push_back(base::SysWideToUTF8(language));
  }
  return result;
#elif BUILDFLAG(IS_MAC)
  base::apple::ScopedCFTypeRef<CFArrayRef> languages(
      CFLocaleCopyPreferredLanguages());
  if (!languages) {
    return base::unexpected(
        "ERR_SYSTEM_ERROR: macOS language preferences are unavailable");
  }
  std::vector<std::string> result;
  const CFIndex count = CFArrayGetCount(languages.get());
  result.reserve(count);
  for (CFIndex i = 0; i < count; ++i) {
    CFStringRef language = base::apple::CFCast<CFStringRef>(
        CFArrayGetValueAtIndex(languages.get(), i));
    if (!language) {
      return base::unexpected(
          "ERR_SYSTEM_ERROR: macOS returned an invalid language preference");
    }
    result.push_back(base::SysCFStringRefToUTF8(language));
  }
  return result;
#elif BUILDFLAG(IS_LINUX) && defined(USE_GLIB)
  // GLib implements LANGUAGE, LC_ALL, LC_MESSAGES, LANG precedence and locale
  // variants. Electron preserves that order, including canonical duplicates.
  const char* const* languages = g_get_language_names();
  std::vector<std::string> result;
  // SAFETY: GLib guarantees a non-null, null-terminated array of C strings.
  for (; *languages; UNSAFE_BUFFERS(++languages)) {
    if (std::string_view(*languages) != "C") {
      result.push_back(base::i18n::GetCanonicalLocale(*languages));
    }
  }
  return result;
#else
  return base::unexpected(
      "ERR_NOT_SUPPORTED: system language preferences are unavailable on this "
      "platform");
#endif
}

}  // namespace xenon::ipc

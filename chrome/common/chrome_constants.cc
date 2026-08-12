// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/chrome_constants.h"

#include "build/build_config.h"
#include "chrome/common/chrome_version.h"

#define FPL FILE_PATH_LITERAL

namespace chrome {

#if defined(CUSTOM_CHROME_VERSION_STRING)
const char kChromeVersion[] = CUSTOM_CHROME_VERSION_STRING;
#else
const char kChromeVersion[] = CHROME_VERSION_STRING;
#endif

// The following should not be used for UI strings; they are meant
// for system strings only. UI changes should be made in the GRD.
//
// There are four constants used to locate the executable name and path:
//
//     kBrowserProcessExecutableName
//     kHelperProcessExecutableName
//     kBrowserProcessExecutablePath
//     kHelperProcessExecutablePath
//
// In one condition, our tests will be built using the Chrome branding
// though we want to actually execute a Chromium branded application.
// This happens for the reference build on Mac.  To support that case,
// we also include a Chromium version of each of the four constants and
// in the UITest class we support switching to that version when told to
// do so.

#if BUILDFLAG(IS_WIN)
const base::FilePath::CharType kBrowserProcessExecutableName[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME);
#else
    FPL("chrome.exe");
#endif
const base::FilePath::CharType kHelperProcessExecutableName[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME);
#else
    FPL("chrome.exe");
#endif
#elif BUILDFLAG(IS_MAC)
const base::FilePath::CharType kBrowserProcessExecutableName[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME);
#else
    FPL(PRODUCT_FULLNAME_STRING);
#endif
const base::FilePath::CharType kHelperProcessExecutableName[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME " Helper");
#else
    FPL(PRODUCT_FULLNAME_STRING " Helper");
#endif
#elif BUILDFLAG(IS_ANDROID)
// NOTE: Keep it synced with the process names defined in AndroidManifest.xml.
const base::FilePath::CharType kBrowserProcessExecutableName[] = FPL("chrome");
const base::FilePath::CharType kHelperProcessExecutableName[] =
    FPL("sandboxed_process");
#elif BUILDFLAG(IS_POSIX)
const base::FilePath::CharType kBrowserProcessExecutableName[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME);
#else
    FPL("chrome");
#endif
// Helper processes end up with a name of "exe" due to execing via
// /proc/self/exe.  See bug 22703.
const base::FilePath::CharType kHelperProcessExecutableName[] = FPL("exe");
#endif  // OS_*

#if BUILDFLAG(IS_WIN)
const base::FilePath::CharType kBrowserProcessExecutablePath[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME);
#else
    FPL("chrome.exe");
#endif
const base::FilePath::CharType kHelperProcessExecutablePath[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME);
#else
    FPL("chrome.exe");
#endif
#elif BUILDFLAG(IS_MAC)
const base::FilePath::CharType kBrowserProcessExecutablePath[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME ".app/Contents/MacOS/" CUSTOM_CHROME_EXE_NAME);
#else
    FPL(PRODUCT_FULLNAME_STRING ".app/Contents/MacOS/" PRODUCT_FULLNAME_STRING);
#endif
const base::FilePath::CharType
    kGoogleChromeForTestingBrowserProcessExecutablePath[] =
        FPL("Google Chrome for Testing.app/Contents/MacOS/Google Chrome for "
            "Testing");
const base::FilePath::CharType kGoogleChromeBrowserProcessExecutablePath[] =
    FPL("Google Chrome.app/Contents/MacOS/Google Chrome");
const base::FilePath::CharType kChromiumBrowserProcessExecutablePath[] =
    FPL("Chromium.app/Contents/MacOS/Chromium");
const base::FilePath::CharType kHelperProcessExecutablePath[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME
        " Helper.app/Contents/MacOS/" CUSTOM_CHROME_EXE_NAME " Helper");
#else
    FPL(PRODUCT_FULLNAME_STRING
        " Helper.app/Contents/MacOS/" PRODUCT_FULLNAME_STRING " Helper");
#endif
#elif BUILDFLAG(IS_ANDROID)
const base::FilePath::CharType kBrowserProcessExecutablePath[] = FPL("chrome");
const base::FilePath::CharType kHelperProcessExecutablePath[] = FPL("chrome");
#elif BUILDFLAG(IS_POSIX)
const base::FilePath::CharType kBrowserProcessExecutablePath[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME);
#else
    FPL("chrome");
#endif
const base::FilePath::CharType kHelperProcessExecutablePath[] =
#if defined(CUSTOM_CHROME_EXE_NAME)
    FPL(CUSTOM_CHROME_EXE_NAME);
#else
    FPL("chrome");
#endif
#endif  // OS_*

#if BUILDFLAG(IS_MAC)
const base::FilePath::CharType kFrameworkName[] =
#if defined(CUSTOM_CHROME_DLL_NAME)
    FPL(CUSTOM_CHROME_DLL_NAME " Framework.framework");
#else
    FPL(PRODUCT_FULLNAME_STRING " Framework.framework");
#endif
const base::FilePath::CharType kFrameworkExecutableName[] =
#if defined(CUSTOM_CHROME_DLL_NAME)
    FPL(CUSTOM_CHROME_DLL_NAME " Framework");
#else
    FPL(PRODUCT_FULLNAME_STRING " Framework");
#endif
const char kMacHelperSuffixAlerts[] = " (Alerts)";
#endif  // BUILDFLAG(IS_MAC)

}  // namespace chrome

#undef FPL

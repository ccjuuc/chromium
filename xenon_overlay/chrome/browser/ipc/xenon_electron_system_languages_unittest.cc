// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_system_languages.h"

#include "base/strings/string_util.h"
#include "base/test/icu_test_util.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_LINUX) && defined(USE_GLIB)
#include "base/scoped_environment_variable_override.h"
#endif

namespace xenon::ipc {
namespace {

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || \
    (BUILDFLAG(IS_LINUX) && defined(USE_GLIB))
TEST(XenonElectronSystemLanguagesTest, ReadsValidSystemLanguages) {
  const auto languages = GetPreferredSystemLanguages();
  ASSERT_TRUE(languages.has_value()) << languages.error();
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  EXPECT_FALSE(languages->empty());
#endif
  for (const auto& language : *languages) {
    EXPECT_FALSE(language.empty());
    EXPECT_TRUE(base::IsStringUTF8(language));
    EXPECT_EQ(language.find('\0'), std::string::npos);
    EXPECT_NE(language, "C");
  }
}

TEST(XenonElectronSystemLanguagesTest, ApplicationLocaleDoesNotOverrideSystem) {
  base::test::ScopedRestoreICUDefaultLocale application_locale("en_US_POSIX");
  const auto before = GetPreferredSystemLanguages();
  ASSERT_TRUE(before.has_value()) << before.error();
  {
    base::test::ScopedRestoreICUDefaultLocale changed_locale("ja_JP");
    EXPECT_EQ(GetPreferredSystemLanguages(), before);
  }
  EXPECT_EQ(GetPreferredSystemLanguages(), before);
}

#else
TEST(XenonElectronSystemLanguagesTest, UnsupportedPlatformReportsError) {
  const auto languages = GetPreferredSystemLanguages();
  ASSERT_FALSE(languages.has_value());
  EXPECT_TRUE(languages.error().starts_with("ERR_NOT_SUPPORTED:"));
}
#endif

#if BUILDFLAG(IS_LINUX) && defined(USE_GLIB)
TEST(XenonElectronSystemLanguagesTest, LanguageListTakesPrecedence) {
  base::ScopedEnvironmentVariableOverride language("LANGUAGE", "fr_CA:de_DE");
  base::ScopedEnvironmentVariableOverride all("LC_ALL", "es_ES");
  base::ScopedEnvironmentVariableOverride messages("LC_MESSAGES", "it_IT");
  base::ScopedEnvironmentVariableOverride lang("LANG", "ja_JP");
  const auto languages = GetPreferredSystemLanguages();
  ASSERT_TRUE(languages.has_value()) << languages.error();
  EXPECT_EQ(*languages,
            (std::vector<std::string>{"fr-CA", "fr", "de-DE", "de"}));
}

TEST(XenonElectronSystemLanguagesTest, LocaleEnvironmentPrecedence) {
  base::ScopedEnvironmentVariableOverride language("LANGUAGE");
  base::ScopedEnvironmentVariableOverride all("LC_ALL", "es_ES");
  base::ScopedEnvironmentVariableOverride messages("LC_MESSAGES", "it_IT");
  base::ScopedEnvironmentVariableOverride lang("LANG", "ja_JP");
  EXPECT_EQ(GetPreferredSystemLanguages().value(),
            (std::vector<std::string>{"es-ES", "es"}));
  {
    base::ScopedEnvironmentVariableOverride unset_all("LC_ALL");
    EXPECT_EQ(GetPreferredSystemLanguages().value(),
              (std::vector<std::string>{"it-IT", "it"}));
    base::ScopedEnvironmentVariableOverride unset_messages("LC_MESSAGES");
    EXPECT_EQ(GetPreferredSystemLanguages().value(),
              (std::vector<std::string>{"ja-JP", "ja"}));
  }
}

TEST(XenonElectronSystemLanguagesTest, CLocaleHasNoLanguagePreference) {
  base::ScopedEnvironmentVariableOverride language("LANGUAGE", "C");
  const auto languages = GetPreferredSystemLanguages();
  ASSERT_TRUE(languages.has_value()) << languages.error();
  EXPECT_TRUE(languages->empty());
}
#endif

}  // namespace
}  // namespace xenon::ipc

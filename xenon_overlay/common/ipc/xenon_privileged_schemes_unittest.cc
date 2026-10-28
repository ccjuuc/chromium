// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_privileged_schemes.h"

#include "base/base_paths.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/test/multiprocess_test.h"
#include "base/test/scoped_path_override.h"
#include "base/test/test_timeouts.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

namespace xenon::ipc {
namespace {

constexpr char kDeclaration[] = R"([
  {"scheme":"sample-app", "privileges":{
    "standard":true,"secure":true,"supportFetchAPI":true,"corsEnabled":true}}
])";
constexpr char kDescriptor[] =
    R"({"application":"unopened-app", "privilegedSchemes":[
  {"scheme":"sample-app", "privileges":{
    "standard":true,"secure":true,"supportFetchAPI":true,"corsEnabled":true}}
]})";
constexpr char kCspDeclaration[] =
    R"([{"scheme":"crx","privileges":{"bypassCSP":true}}])";
constexpr char kSnapshotDescriptor[] =
    R"({"application":"unopened-app", "privilegedSchemes":[
  {"scheme":"sample-app", "privileges":{
    "standard":true,"secure":true,"supportFetchAPI":true,"corsEnabled":true}},
  {"scheme":"crx", "privileges":{"bypassCSP":true}}
]})";

base::Value Json(std::string_view text) {
  auto value = base::JSONReader::Read(text, base::JSON_PARSE_RFC);
  CHECK(value);
  return std::move(*value);
}

TEST(PrivilegedSchemesTest, NormalizesAndDeduplicatesIdenticalDeclarations) {
  PrivilegedSchemes parsed;
  std::string error;
  EXPECT_TRUE(ParsePrivilegedSchemes(Json(R"([
    {"scheme":"Z-app", "privileges":{"secure":true}},
    {"scheme":"a-app"},
    {"scheme":"z-app", "privileges":{"secure":true,"stream":false}}
  ])"),
                                     &parsed, &error))
      << error;
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].scheme, "a-app");
  EXPECT_FALSE(parsed[0].secure);
  EXPECT_EQ(parsed[1].scheme, "z-app");
  EXPECT_TRUE(parsed[1].secure);
  EXPECT_TRUE(error.empty());
}

TEST(PrivilegedSchemesTest, CspBypassDoesNotImplyOtherPrivileges) {
  PrivilegedSchemes parsed;
  std::string error;
  ASSERT_TRUE(ParsePrivilegedSchemes(Json(kCspDeclaration), &parsed, &error))
      << error;
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].scheme, "crx");
  EXPECT_TRUE(parsed[0].bypass_csp);
  EXPECT_FALSE(parsed[0].standard);
  EXPECT_FALSE(parsed[0].secure);
  EXPECT_FALSE(parsed[0].support_fetch_api);
  EXPECT_FALSE(parsed[0].cors_enabled);

  ASSERT_TRUE(ParsePrivilegedSchemes(Json(kDeclaration), &parsed, &error))
      << error;
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_FALSE(parsed[0].bypass_csp);
}

TEST(PrivilegedSchemesTest,
     RejectsMalformedAndUnsupportedDeclarationsAtomically) {
  for (
      const auto* text :
      {"{}", "null", "[1]", "[{}]", "[{\"scheme\":1}]",
       R"([{"scheme":"sample","extra":true}])",
       R"([{"scheme":"sample:","privileges":{}}])", R"([{"scheme":"1sample"}])",
       R"([{"scheme":"sam ple"}])",
       R"([{"scheme":"sample","privileges":null}])",
       R"([{"scheme":"sample","privileges":{"secure":1}}])",
       R"([{"scheme":"sample","privileges":{"unknown":false}}])",
       R"([{"scheme":"sample","privileges":{"bypassCSP":"true"}}])",
       R"([{"scheme":"sample","privileges":{"allowServiceWorkers":true}}])",
       R"([{"scheme":"sample","privileges":{"stream":true}}])",
       R"([{"scheme":"sample"},{"scheme":"SAMPLE","privileges":{"secure":true}}])",
       R"([{"scheme":"crx"},{"scheme":"CRX","privileges":{"bypassCSP":true}}])"}) {
    PrivilegedSchemes parsed = {{.scheme = "unchanged"}};
    std::string error;
    EXPECT_FALSE(ParsePrivilegedSchemes(Json(text), &parsed, &error)) << text;
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].scheme, "unchanged");
    EXPECT_FALSE(error.empty());
  }
}

TEST(PrivilegedSchemesTest, RejectsReservedSchemesCaseInsensitively) {
  for (const auto* scheme :
       {"HTTP", "https", "file", "blob", "data", "about", "javascript", "ws",
        "wss", "chrome", "chrome-extension", "devtools", "isolated-app"}) {
    PrivilegedSchemes parsed;
    std::string error;
    base::ListValue list;
    list.Append(base::DictValue()
                    .Set("scheme", scheme)
                    .Set("privileges",
                         base::DictValue().Set("bypassCSP", true)));
    EXPECT_FALSE(
        ParsePrivilegedSchemes(base::Value(std::move(list)), &parsed, &error))
        << scheme;
    EXPECT_NE(error.find("reserved"), std::string::npos);
  }
}

TEST(PrivilegedSchemesTest, BoundsTransportSize) {
  base::ListValue list;
  for (int i = 0; i < 33; ++i) {
    list.Append(base::DictValue().Set("scheme", "scheme" + std::to_string(i)));
  }
  PrivilegedSchemes parsed;
  std::string error;
  EXPECT_FALSE(
      ParsePrivilegedSchemes(base::Value(std::move(list)), &parsed, &error));
  EXPECT_FALSE(ParsePrivilegedSchemes(
      base::Value(base::ListValue().Append(
          base::DictValue().Set("scheme", std::string(65, 'a')))),
      &parsed, &error));
}

TEST(PrivilegedSchemesTest, LoadsOnlyHostDescriptorsAndExplicitJson) {
  base::ScopedTempDir host;
  base::ScopedTempDir external;
  ASSERT_TRUE(host.CreateUniqueTempDir());
  ASSERT_TRUE(external.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(host.GetPath().AppendASCII("one.xenon.json"),
                              kDescriptor));
  ASSERT_TRUE(
      base::WriteFile(host.GetPath().AppendASCII("ignored.json"), "bad"));
  ASSERT_TRUE(base::CreateDirectory(host.GetPath().AppendASCII("nested")));
  ASSERT_TRUE(base::WriteFile(
      host.GetPath().AppendASCII("nested").AppendASCII("ignored.xenon.json"),
      "bad"));
  const auto explicit_path = external.GetPath().AppendASCII("selected.json");
  ASSERT_TRUE(base::WriteFile(explicit_path, R"({"privilegedSchemes":[
    {"scheme":"second-app","privileges":{"standard":true}}
  ]})"));
  PrivilegedSchemes parsed;
  std::string error;
  ASSERT_TRUE(LoadPrivilegedSchemeDeclarations(host.GetPath(), explicit_path,
                                               &parsed, &error))
      << error;
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].scheme, "sample-app");
  EXPECT_EQ(parsed[1].scheme, "second-app");
  EXPECT_TRUE(LoadPrivilegedSchemeDeclarations(
      host.GetPath(), external.GetPath(), &parsed, &error));
  EXPECT_EQ(parsed.size(), 1u);
}

TEST(PrivilegedSchemesTest,
     ConflictingFilesAndInvalidDescriptorsFailAtomically) {
  base::ScopedTempDir host;
  ASSERT_TRUE(host.CreateUniqueTempDir());
  ASSERT_TRUE(
      base::WriteFile(host.GetPath().AppendASCII("a.xenon.json"), kDescriptor));
  const auto conflict = host.GetPath().AppendASCII("b.xenon.json");
  ASSERT_TRUE(base::WriteFile(
      conflict, R"({"privilegedSchemes":[{"scheme":"sample-app"}]})"));
  PrivilegedSchemes parsed = {{.scheme = "unchanged"}};
  std::string error;
  EXPECT_FALSE(
      LoadPrivilegedSchemeDeclarations(host.GetPath(), {}, &parsed, &error));
  EXPECT_NE(error.find("Conflicting"), std::string::npos);
  EXPECT_EQ(parsed[0].scheme, "unchanged");
  ASSERT_TRUE(base::WriteFile(conflict, R"({"privilegedSchemes":{}})"));
  EXPECT_FALSE(
      LoadPrivilegedSchemeDeclarations(host.GetPath(), {}, &parsed, &error));
  EXPECT_NE(error.find("b.xenon.json"), std::string::npos);
  EXPECT_FALSE(LoadPrivilegedSchemeDeclarations(
      {}, base::FilePath(FILE_PATH_LITERAL("relative.json")), &parsed, &error));
}

int RunChild(const char* name, const base::CommandLine& command_line) {
  base::LaunchOptions options;
#if BUILDFLAG(IS_WIN)
  options.start_hidden = true;
#endif
  auto process = base::SpawnMultiProcessTestChild(name, command_line, options);
  int exit_code = -1;
  EXPECT_TRUE(process.IsValid());
  EXPECT_TRUE(base::WaitForMultiprocessTestChildExit(
      process, TestTimeouts::action_timeout(), &exit_code));
  return exit_code;
}

MULTIPROCESS_TEST_MAIN(PrivilegedSchemeSnapshotChild) {
  InitializeStartupPrivilegedSchemes();
  std::string error;
  EXPECT_TRUE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kDeclaration).GetList(), &error))
      << error;
  EXPECT_TRUE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kCspDeclaration).GetList(), &error))
      << error;
  EXPECT_FALSE(ValidatePrivilegedSchemesAgainstStartup(
      Json(R"([{"scheme":"crx","privileges":{"bypassCSP":false}}])")
          .GetList(),
      &error));
  EXPECT_FALSE(ValidatePrivilegedSchemesAgainstStartup(
      Json(R"([{"scheme":"sample-app","privileges":{
        "standard":true,"secure":true,"supportFetchAPI":true,
        "corsEnabled":true,"bypassCSP":true}}])")
          .GetList(),
      &error));
  EXPECT_FALSE(ValidatePrivilegedSchemesAgainstStartup(
      Json(R"([{"scheme":"sample-app"}])").GetList(), &error));
  EXPECT_FALSE(ValidatePrivilegedSchemesAgainstStartup(
      Json(R"([{"scheme":"not-at-startup"}])").GetList(), &error));
  EXPECT_EQ(GetStartupPrivilegedSchemes().size(), 2u);
  if (GetStartupPrivilegedSchemes().size() != 2u) {
    return 1;
  }
  EXPECT_EQ(GetStartupPrivilegedSchemes()[0].scheme, "crx");
  EXPECT_TRUE(GetStartupPrivilegedSchemes()[0].bypass_csp);
  EXPECT_FALSE(GetStartupPrivilegedSchemes()[1].bypass_csp);
  return testing::Test::HasFailure() ? 1 : 0;
}

MULTIPROCESS_TEST_MAIN(PrivilegedSchemeSnapshotBrowser) {
  base::ScopedTempDir host;
  CHECK(host.CreateUniqueTempDir());
  base::ScopedPathOverride override(base::DIR_EXE, host.GetPath());
  auto* command_line = base::CommandLine::ForCurrentProcess();
  const auto descriptor = host.GetPath().AppendASCII("selected.json");
  CHECK(base::WriteFile(descriptor, kSnapshotDescriptor));
  command_line->AppendSwitchPath(switches::kElectronApp, descriptor);
  std::string error;
  EXPECT_FALSE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kDeclaration).GetList(), &error));
  InitializeStartupPrivilegedSchemes();
  EXPECT_TRUE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kDeclaration).GetList(), &error))
      << error;
  EXPECT_TRUE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kCspDeclaration).GetList(), &error))
      << error;
  // A later on-disk edit must neither change the browser nor future children.
  CHECK(base::WriteFile(descriptor, "invalid JSON after startup"));
  InitializeStartupPrivilegedSchemes();
  EXPECT_TRUE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kDeclaration).GetList(), &error));
  EXPECT_TRUE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kCspDeclaration).GetList(), &error));
  auto child = base::GetMultiProcessTestChildBaseCommandLine();
  child.RemoveSwitch("test-child-process");
  child.AppendSwitchASCII("type", "utility");
  AppendStartupPrivilegedSchemes(&child);
  EXPECT_EQ(RunChild("PrivilegedSchemeSnapshotChild", child), 0);
  return testing::Test::HasFailure() ? 1 : 0;
}

TEST(PrivilegedSchemesTest, FreezesBrowserSnapshotAndPropagatesToChildren) {
  auto command_line = base::GetMultiProcessTestChildBaseCommandLine();
  command_line.RemoveSwitch("type");
  EXPECT_EQ(RunChild("PrivilegedSchemeSnapshotBrowser", command_line), 0);
}

MULTIPROCESS_TEST_MAIN(PrivilegedSchemeInvalidSnapshot) {
  InitializeStartupPrivilegedSchemes();
  EXPECT_TRUE(GetStartupPrivilegedSchemes().empty());
  std::string error;
  EXPECT_FALSE(ValidatePrivilegedSchemesAgainstStartup({}, &error));
  EXPECT_FALSE(error.empty());
  return testing::Test::HasFailure() ? 1 : 0;
}

TEST(PrivilegedSchemesTest, InvalidChildSnapshotFailsClosed) {
  auto command_line = base::GetMultiProcessTestChildBaseCommandLine();
  command_line.AppendSwitchASCII("type", "utility");
  command_line.AppendSwitchASCII("xenon-privileged-schemes-snapshot",
                                 "not base64");
  EXPECT_EQ(RunChild("PrivilegedSchemeInvalidSnapshot", command_line), 0);
}

}  // namespace
}  // namespace xenon::ipc

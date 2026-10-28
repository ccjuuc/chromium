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
#include "xenon_overlay/common/ipc/xenon_app_descriptor_paths.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#endif

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
  ASSERT_TRUE(LoadPrivilegedSchemeDeclarations({host.GetPath()}, explicit_path,
                                               &parsed, &error))
      << error;
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].scheme, "sample-app");
  EXPECT_EQ(parsed[1].scheme, "second-app");
  EXPECT_TRUE(LoadPrivilegedSchemeDeclarations(
      {host.GetPath()}, external.GetPath(), &parsed, &error));
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
      LoadPrivilegedSchemeDeclarations({host.GetPath()}, {}, &parsed, &error));
  EXPECT_NE(error.find("Conflicting"), std::string::npos);
  EXPECT_EQ(parsed[0].scheme, "unchanged");
  ASSERT_TRUE(base::WriteFile(conflict, R"({"privilegedSchemes":{}})"));
  EXPECT_FALSE(
      LoadPrivilegedSchemeDeclarations({host.GetPath()}, {}, &parsed, &error));
  EXPECT_NE(error.find("b.xenon.json"), std::string::npos);
  EXPECT_FALSE(LoadPrivilegedSchemeDeclarations(
      {}, base::FilePath(FILE_PATH_LITERAL("relative.json")), &parsed, &error));
}

TEST(PrivilegedSchemesTest, ResourceDescriptorWinsWithoutDroppingLegacyApps) {
  base::ScopedTempDir resources;
  base::ScopedTempDir legacy;
  ASSERT_TRUE(resources.CreateUniqueTempDir());
  ASSERT_TRUE(legacy.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(
      resources.GetPath().AppendASCII("application.xenon.json"), kDescriptor));
  ASSERT_TRUE(base::WriteFile(
      legacy.GetPath().AppendASCII("application.xenon.json"), "stale invalid"));
  ASSERT_TRUE(
      base::WriteFile(legacy.GetPath().AppendASCII("other.xenon.json"),
                      R"({"privilegedSchemes":[{"scheme":"legacy-app"}]})"));
  PrivilegedSchemes parsed;
  std::string error;
  ASSERT_TRUE(LoadPrivilegedSchemeDeclarations(
      {resources.GetPath(), legacy.GetPath()}, {}, &parsed, &error))
      << error;
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].scheme, "legacy-app");
  EXPECT_EQ(parsed[1].scheme, "sample-app");
  // A broken preferred descriptor must fail, not silently reuse an older
  // policy.
  EXPECT_FALSE(LoadPrivilegedSchemeDeclarations(
      {legacy.GetPath(), resources.GetPath()}, {}, &parsed, &error));
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

MULTIPROCESS_TEST_MAIN(ElectronDescriptorUnbundledPaths) {
  base::ScopedTempDir host;
  CHECK(host.CreateUniqueTempDir());
  base::FilePath host_directory;
  CHECK(base::NormalizeFilePath(host.GetPath(), &host_directory));
  base::ScopedPathOverride override(base::DIR_EXE, host_directory);
#if BUILDFLAG(IS_MAC)
  base::apple::SetOverrideAmIBundled(false);
#endif
  EXPECT_EQ(GetElectronAppDescriptorDirectories(),
            std::vector<base::FilePath>({host_directory}));
  const auto descriptor = host_directory.AppendASCII("application.xenon.json");
  CHECK(base::WriteFile(descriptor, kDescriptor));
  EXPECT_EQ(FindElectronAppDescriptor("application.xenon.json"), descriptor);
  EXPECT_TRUE(FindElectronAppDescriptor("missing.xenon.json").empty());
  EXPECT_TRUE(FindElectronAppDescriptor("../application.xenon.json").empty());
  return testing::Test::HasFailure() ? 1 : 0;
}

TEST(PrivilegedSchemesTest, UnbundledHostsKeepExecutableDirectoryDiscovery) {
  auto command_line = base::GetMultiProcessTestChildBaseCommandLine();
  EXPECT_EQ(RunChild("ElectronDescriptorUnbundledPaths", command_line), 0);
}

#if BUILDFLAG(IS_MAC)
MULTIPROCESS_TEST_MAIN(ElectronDescriptorBundledPaths) {
  base::ScopedTempDir host;
  CHECK(host.CreateUniqueTempDir());
  base::FilePath host_directory;
  CHECK(base::NormalizeFilePath(host.GetPath(), &host_directory));
  const auto bundle = host_directory.AppendASCII("Host.app");
  const auto contents = bundle.AppendASCII("Contents");
  const auto executable_directory = contents.AppendASCII("MacOS");
  const auto resources =
      contents.AppendASCII("Resources").AppendASCII("xenon-apps");
  CHECK(base::CreateDirectory(executable_directory));
  CHECK(base::CreateDirectory(resources));
  CHECK(base::WriteFile(contents.AppendASCII("Info.plist"), R"(
    <plist version="1.0"><dict>
      <key>CFBundleIdentifier</key><string>org.xenon.test-host</string>
      <key>CFBundlePackageType</key><string>APPL</string>
    </dict></plist>)"));
  base::ScopedPathOverride override(base::DIR_EXE, executable_directory);
  base::apple::SetOverrideOuterBundlePath(bundle);
  base::apple::SetOverrideAmIBundled(true);
  const auto normalize = [](const base::FilePath& path) {
    base::FilePath normalized;
    CHECK(base::NormalizeFilePath(path, &normalized));
    return normalized;
  };
  auto directories = GetElectronAppDescriptorDirectories();
  for (auto& directory : directories) {
    directory = normalize(directory);
  }
  EXPECT_EQ(directories,
            std::vector<base::FilePath>(
                {normalize(resources), normalize(executable_directory)}));
  const auto preferred = resources.AppendASCII("application.xenon.json");
  CHECK(base::WriteFile(preferred, kSnapshotDescriptor));
  CHECK(base::WriteFile(
      executable_directory.AppendASCII("application.xenon.json"),
      "ignored stale policy"));
  const auto legacy = executable_directory.AppendASCII("legacy.xenon.json");
  CHECK(base::WriteFile(legacy,
                        R"({"privilegedSchemes":[{"scheme":"legacy-app"}]})"));
  EXPECT_EQ(normalize(FindElectronAppDescriptor("application.xenon.json")),
            normalize(preferred));
  EXPECT_EQ(normalize(FindElectronAppDescriptor("legacy.xenon.json")),
            normalize(legacy));
  const auto explicit_descriptor = host_directory.AppendASCII("selected.json");
  CHECK(
      base::WriteFile(explicit_descriptor,
                      R"({"privilegedSchemes":[{"scheme":"explicit-app"}]})"));
  auto* command_line = base::CommandLine::ForCurrentProcess();
  command_line->RemoveSwitch("type");
  command_line->AppendSwitchPath(switches::kElectronApp, explicit_descriptor);
  InitializeStartupPrivilegedSchemes();
  std::string error;
  EXPECT_TRUE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kDeclaration).GetList(), &error))
      << error;
  EXPECT_TRUE(ValidatePrivilegedSchemesAgainstStartup(
      Json(kCspDeclaration).GetList(), &error))
      << error;
  EXPECT_EQ(GetStartupPrivilegedSchemes().size(), 4u);
  return testing::Test::HasFailure() ? 1 : 0;
}

TEST(PrivilegedSchemesTest,
     BundledMacHostsRegisterResourcesLegacyAndExplicitDescriptors) {
  auto command_line = base::GetMultiProcessTestChildBaseCommandLine();
  EXPECT_EQ(RunChild("ElectronDescriptorBundledPaths", command_line), 0);
}
#endif

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
      Json(R"([{"scheme":"crx","privileges":{"bypassCSP":false}}])").GetList(),
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

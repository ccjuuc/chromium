// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_downloader.h"

#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/updater/xenon_updater_test_support.h"

namespace xenon::updater {

namespace {

constexpr char kAbcHash[] =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

base::DictValue PackageValue() {
  return base::DictValue().Set("url", "http://127.0.0.1/package.zip")
      .Set("size", 3).Set("sha256", kAbcHash);
}

TEST(XenonUpdatePackageTest, RequiresCompleteHexDigest) {
  EXPECT_TRUE(UpdatePackageInfo::FromValue(PackageValue()).has_value());
  for (const std::string& hash : {std::string(), std::string("abcd"),
                                  std::string(64, 'g'), std::string(65, 'a')}) {
    auto value = PackageValue();
    value.Set("sha256", hash);
    EXPECT_FALSE(UpdatePackageInfo::FromValue(value).has_value());
  }
  auto uppercase = PackageValue();
  uppercase.Set("sha256", std::string(64, 'A'));
  EXPECT_TRUE(UpdatePackageInfo::FromValue(uppercase).has_value());
}

TEST(XenonUpdatePackageTest, RejectsUnsafeOrFractionalSizeBeforeConversion) {
  for (double size : {0.0, -1.0, 3.5, 9007199254740992.0, 1e100}) {
    auto value = PackageValue();
    value.Set("size", size);
    EXPECT_FALSE(UpdatePackageInfo::FromValue(value).has_value()) << size;
  }
  auto large = PackageValue();
  large.Set("size", 9007199254740991.0);
  EXPECT_TRUE(UpdatePackageInfo::FromValue(large).has_value());
}

class XenonUpdateDownloaderTest : public testing::Test {
 protected:
  void SetUp() override {
    InitializeUpdaterMojoForTesting();
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    destination_ = directory_.GetPath().AppendASCII("package.zip");
    package_ = *UpdatePackageInfo::FromValue(PackageValue());
  }

  void Start(base::OnceCallback<void(bool, const std::string&)> callback) {
    downloader_.StartDownload(factory_.GetSafeWeakWrapper(), package_,
                              destination_, {}, std::move(callback));
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  base::ScopedTempDir directory_;
  base::FilePath destination_;
  network::TestURLLoaderFactory factory_;
  UpdatePackageInfo package_;
  XenonUpdateDownloader downloader_;
};

TEST_F(XenonUpdateDownloaderTest, VerifiesHashAndSize) {
  factory_.AddResponse(package_.url.spec(), "abc");
  base::test::TestFuture<bool, std::string> completed;
  Start(completed.GetCallback<bool, const std::string&>());
  ASSERT_TRUE(completed.Wait());
  EXPECT_TRUE(completed.Get<0>()) << completed.Get<1>();
  std::string contents;
  EXPECT_TRUE(base::ReadFileToString(destination_, &contents));
  EXPECT_EQ(contents, "abc");
}

TEST_F(XenonUpdateDownloaderTest, RejectsBothShortAndOversizedBodies) {
  for (int64_t size : {2, 4}) {
    factory_.AddResponse(package_.url.spec(), "abc");
    package_.size = size;
    base::test::TestFuture<bool, std::string> completed;
    Start(completed.GetCallback<bool, const std::string&>());
    ASSERT_TRUE(completed.Wait());
    EXPECT_FALSE(completed.Get<0>());
    EXPECT_FALSE(base::PathExists(destination_));
  }
}

TEST_F(XenonUpdateDownloaderTest, CancellationDoesNotRunFallbackCallback) {
  base::test::TestFuture<bool, std::string> cancelled;
  Start(cancelled.GetCallback<bool, const std::string&>());
  downloader_.Cancel();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(cancelled.IsReady());
  EXPECT_FALSE(downloader_.is_downloading());
}

TEST_F(XenonUpdateDownloaderTest, ReplacementDoesNotNotifyPreviousRequest) {
  base::test::TestFuture<bool, std::string> replaced;
  Start(replaced.GetCallback<bool, const std::string&>());
  package_.url = GURL("http://127.0.0.1/replacement.zip");
  factory_.AddResponse(package_.url.spec(), "abc");
  base::test::TestFuture<bool, std::string> completed;
  Start(completed.GetCallback<bool, const std::string&>());
  ASSERT_TRUE(completed.Wait());
  EXPECT_TRUE(completed.Get<0>()) << completed.Get<1>();
  EXPECT_FALSE(replaced.IsReady());
}

TEST_F(XenonUpdateDownloaderTest, CorruptFileIsRemovedBeforeFallbackStarts) {
  factory_.AddResponse(package_.url.spec(), "abc");
  package_.sha256 = std::string(64, '0');
  base::test::TestFuture<bool, std::string> fallback;
  Start(base::BindLambdaForTesting([&](bool success, const std::string& error) {
    EXPECT_FALSE(success);
    EXPECT_FALSE(base::PathExists(destination_));
    package_.sha256 = kAbcHash;
    Start(fallback.GetCallback<bool, const std::string&>());
  }));
  ASSERT_TRUE(fallback.Wait());
  EXPECT_TRUE(fallback.Get<0>()) << fallback.Get<1>();
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(base::PathExists(destination_));
}

}  // namespace
}  // namespace xenon::updater

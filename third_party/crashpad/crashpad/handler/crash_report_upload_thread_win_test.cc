// Copyright 2026 The Crashpad Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// https://www.apache.org/licenses/LICENSE-2.0

#include "handler/crash_report_upload_thread.h"

#include <stdint.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "base/strings/stringprintf.h"
#include "client/crash_report_database.h"
#include "client/settings.h"
#include "gtest/gtest.h"
#include "test/multiprocess_exec.h"
#include "test/scoped_temp_dir.h"
#include "test/test_paths.h"
#include "util/file/file_io.h"
#include "util/file/file_writer.h"

namespace crashpad::test {
namespace {

class UploadToServer final : public MultiprocessExec {
 public:
  explicit UploadToServer(bool binary,
                          bool gzip = false,
                          int previous_failures = 0)
      : binary_(binary),
        gzip_(gzip),
        previous_failures_(previous_failures) {
    SetChildCommand(TestPaths::Executable().DirName().Append(
                        FILE_PATH_LITERAL("http_transport_test_server.exe")),
                    nullptr);
  }

 private:
  void MultiprocessParent() override {
    uint16_t port;
    ASSERT_TRUE(LoggingReadFileExactly(ReadPipeHandle(), &port, sizeof(port)));
    const uint16_t status = 200;
    ASSERT_TRUE(LoggingWriteFile(WritePipeHandle(), &status, sizeof(status)));
    const std::string remote_id = "0123456789abcdef";
    ASSERT_TRUE(LoggingWriteFile(WritePipeHandle(), remote_id.data(), 16));

    ScopedTempDir temp_dir;
    auto database = CrashReportDatabase::Initialize(temp_dir.path());
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->GetSettings()->SetUploadsEnabled(true));
    std::unique_ptr<CrashReportDatabase::NewReport> report;
    ASSERT_EQ(database->PrepareNewCrashReport(&report),
              CrashReportDatabase::kNoError);
    const std::string dump("test\0dump", 9);
    ASSERT_TRUE(report->Writer()->Write(dump.data(), dump.size()));
    UUID uuid;
    ASSERT_EQ(database->FinishedWritingCrashReport(std::move(report), &uuid),
              CrashReportDatabase::kNoError);

    for (int attempt = 0; attempt < previous_failures_; ++attempt) {
      std::unique_ptr<const CrashReportDatabase::UploadReport> failed_report;
      ASSERT_EQ(database->GetReportForUploading(uuid, &failed_report),
                CrashReportDatabase::kNoError);
      // Releasing the upload report records a failed attempt, including the
      // timestamp, without waiting for a real network timeout or backoff.
    }
    if (previous_failures_) {
      ASSERT_EQ(database->SkipReportUpload(
                    uuid, Metrics::CrashSkippedReason::kUploadFailed),
                CrashReportDatabase::kNoError);
      database.reset();
      database = CrashReportDatabase::Initialize(temp_dir.path());
      ASSERT_TRUE(database);
      ASSERT_EQ(database->RequestUpload(uuid), CrashReportDatabase::kNoError);
    }

    std::promise<void> scanned;
    auto finished = scanned.get_future();
    CrashReportUploadThread::Options options{};
    options.upload_raw = binary_;
    // Binary mode must ignore gzip and send the original dump bytes.
    options.upload_gzip = binary_ || gzip_;
    options.watch_pending_reports = true;
    options.rate_limit = true;
    CrashReportUploadThread uploader(
        database.get(), base::StringPrintf("http://localhost:%u/upload", port),
        options, [&] { scanned.set_value(); });
    uploader.Start();
    const auto result = finished.wait_for(std::chrono::seconds(10));
    uploader.Stop();
    ASSERT_EQ(result, std::future_status::ready);

    std::vector<CrashReportDatabase::Report> completed;
    ASSERT_EQ(database->GetCompletedReports(&completed),
              CrashReportDatabase::kNoError);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_TRUE(completed[0].uploaded);
    EXPECT_EQ(completed[0].id, remote_id + "\r\n");
    EXPECT_EQ(completed[0].upload_attempts, previous_failures_ + 1);
    EXPECT_FALSE(completed[0].upload_explicitly_requested);

    std::string request;
    char buffer[1024];
    FileOperationResult bytes;
    while ((bytes = ReadFile(ReadPipeHandle(), buffer, sizeof(buffer))) != 0) {
      ASSERT_GT(bytes, 0);
      request.append(buffer, bytes);
    }
    if (binary_) {
      EXPECT_NE(request.find("Content-Type: application/octet-stream"),
                std::string::npos);
      const auto body = request.find("\r\n\r\n");
      ASSERT_NE(body, std::string::npos);
      EXPECT_EQ(request.substr(body + 4), dump);
      EXPECT_EQ(request.find("Content-Encoding: gzip"), std::string::npos);
    } else {
      EXPECT_NE(request.find("Content-Type: multipart/form-data;"),
                std::string::npos);
      EXPECT_NE(request.find("upload_file_minidump"), std::string::npos);
      EXPECT_NE(request.find("filename=\"" + uuid.ToString() + ".dmp\""),
                std::string::npos);
      EXPECT_NE(request.find(dump), std::string::npos);
      if (gzip_) {
        EXPECT_NE(request.find("Content-Encoding: gzip"), std::string::npos);
      }
    }
  }

  const bool binary_;
  const bool gzip_;
  const int previous_failures_;
};

TEST(CrashReportUploadThreadTest, BinaryUpload) {
  UploadToServer(true).Run();
}

TEST(CrashReportUploadThreadTest, MultipartUpload) {
  UploadToServer(false).Run();
}

TEST(CrashReportUploadThreadTest, MultipartGzipUpload) {
  UploadToServer(false, true).Run();
}

TEST(CrashReportUploadThreadTest, ManualUploadBypassesBackoffAfterRestart) {
  UploadToServer(false, false, 1).Run();
}

TEST(CrashReportUploadThreadTest, ManualUploadBypassesExhaustionAfterRestart) {
  UploadToServer(false, false, 5).Run();
}

TEST(CrashReportUploadThreadTest, FailedManualRequestIsConsumed) {
  ScopedTempDir temp_dir;
  auto database = CrashReportDatabase::Initialize(temp_dir.path());
  ASSERT_TRUE(database);
  ASSERT_TRUE(database->GetSettings()->SetUploadsEnabled(true));
  std::unique_ptr<CrashReportDatabase::NewReport> report;
  ASSERT_EQ(database->PrepareNewCrashReport(&report),
            CrashReportDatabase::kNoError);
  ASSERT_TRUE(report->Writer()->Write("test dump", 9));
  UUID uuid;
  ASSERT_EQ(database->FinishedWritingCrashReport(std::move(report), &uuid),
            CrashReportDatabase::kNoError);

  for (int pass = 0; pass < 3; ++pass) {
    // The middle scan has no new user request and must not retry the report.
    if (pass != 1) {
      ASSERT_EQ(database->RequestUpload(uuid), CrashReportDatabase::kNoError);
    }
    std::promise<void> scanned;
    auto finished = scanned.get_future();
    CrashReportUploadThread::Options options{};
    options.rate_limit = true;
    options.watch_pending_reports = true;
    CrashReportUploadThread uploader(database.get(), "http://127.0.0.1:0/",
                                     options, [&] { scanned.set_value(); });
    uploader.Start();
    const auto status = finished.wait_for(std::chrono::seconds(10));
    uploader.Stop();
    ASSERT_EQ(status, std::future_status::ready);

    std::vector<CrashReportDatabase::Report> completed;
    ASSERT_EQ(database->GetCompletedReports(&completed),
              CrashReportDatabase::kNoError);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_FALSE(completed[0].uploaded);
    EXPECT_FALSE(completed[0].upload_explicitly_requested);
    EXPECT_EQ(completed[0].upload_attempts, pass == 2 ? 2 : 1);
    std::vector<CrashReportDatabase::Report> pending;
    ASSERT_EQ(database->GetPendingReports(&pending),
              CrashReportDatabase::kNoError);
    EXPECT_TRUE(pending.empty());
  }
}

TEST(CrashReportUploadThreadTest, FailureAndThrottleRemainPendingAfterRestart) {
  ScopedTempDir temp_dir;
  auto database = CrashReportDatabase::Initialize(temp_dir.path());
  ASSERT_TRUE(database);
  ASSERT_TRUE(database->GetSettings()->SetUploadsEnabled(true));

  for (int i = 0; i < 2; ++i) {
    std::unique_ptr<CrashReportDatabase::NewReport> report;
    ASSERT_EQ(database->PrepareNewCrashReport(&report),
              CrashReportDatabase::kNoError);
    // Uploading is allowed even when a dump cannot be parsed for annotations.
    ASSERT_TRUE(report->Writer()->Write("test dump", 9));
    UUID uuid;
    ASSERT_EQ(database->FinishedWritingCrashReport(std::move(report), &uuid),
              CrashReportDatabase::kNoError);
  }

  for (int pass = 0; pass < 2; ++pass) {
    if (pass) {
      database.reset();
      database = CrashReportDatabase::Initialize(temp_dir.path());
      ASSERT_TRUE(database);
    }
    std::promise<void> scanned;
    auto finished = scanned.get_future();
    CrashReportUploadThread::Options options{};
    options.rate_limit = true;
    options.watch_pending_reports = true;
    // Port zero cannot host a receiver. The failed request must not retire
    // the report, and the global throttle must not retire the other report.
    CrashReportUploadThread uploader(database.get(), "http://127.0.0.1:0/",
                                     options, [&] { scanned.set_value(); });
    uploader.Start();
    const auto status = finished.wait_for(std::chrono::seconds(10));
    uploader.Stop();
    ASSERT_EQ(status, std::future_status::ready);

    std::vector<CrashReportDatabase::Report> pending;
    ASSERT_EQ(database->GetPendingReports(&pending),
              CrashReportDatabase::kNoError);
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending[0].upload_attempts + pending[1].upload_attempts, 1);
    std::vector<CrashReportDatabase::Report> completed;
    ASSERT_EQ(database->GetCompletedReports(&completed),
              CrashReportDatabase::kNoError);
    EXPECT_TRUE(completed.empty());
  }
}

}  // namespace
}  // namespace crashpad::test

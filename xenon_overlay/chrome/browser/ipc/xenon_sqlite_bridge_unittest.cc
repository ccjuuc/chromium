// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_sqlite_bridge.h"

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class XenonSqliteBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    bridge_ = std::make_unique<XenonSqliteBridge>();
  }

  mojom::IpcResultPtr Call(const char* operation,
                           base::DictValue options = {}) {
    options.Set("operation", operation);
    if (!options.contains("databaseId")) {
      options.Set("databaseId", database_id_);
    }
    base::ListValue args;
    args.Append(std::move(options));
    return bridge_->Call(base::Value(std::move(args)));
  }

  void Open(const std::string& path = ":memory:", int mode = 6) {
    base::DictValue options;
    options.Set("path", path);
    options.Set("mode", mode);
    auto result = Call("open", std::move(options));
    ASSERT_TRUE(result->success) << result->error;
    database_id_ = result->value.GetInt();
  }

  void Exec(const std::string& sql) {
    base::DictValue options;
    options.Set("sql", sql);
    auto result = Call("exec", std::move(options));
    ASSERT_TRUE(result->success) << result->error;
  }

  int Prepare(const std::string& sql) {
    base::DictValue options;
    options.Set("sql", sql);
    auto result = Call("prepare", std::move(options));
    EXPECT_TRUE(result->success) << result->error;
    return result->success ? result->value.GetInt() : 0;
  }

  mojom::IpcResultPtr Statement(const char* operation, int statement,
                                base::Value params = base::Value()) {
    base::DictValue options;
    options.Set("statementId", statement);
    if (!params.is_none()) {
      options.Set("params", std::move(params));
    }
    return Call(operation, std::move(options));
  }

  void ExpectOk(mojom::IpcResultPtr result) {
    ASSERT_TRUE(result->success) << result->error;
  }

  std::unique_ptr<XenonSqliteBridge> bridge_;
  base::ScopedTempDir temp_;
  int database_id_ = 0;
};

TEST_F(XenonSqliteBridgeTest, ReopenPreservesColumnsTextBlobAndNull) {
  const auto file = temp_.GetPath().AppendASCII("records.db");
  Open(file.AsUTF8Unsafe());
  Exec("CREATE TABLE records(id INTEGER PRIMARY KEY, label TEXT, payload BLOB, "
       "expires INTEGER, optional TEXT)");
  const int insert = Prepare("INSERT INTO records VALUES(?, ?, ?, ?, ?)");
  base::DictValue blob;
  const std::string bytes("\0\xff\x01\0", 4);
  blob.Set("__xenon_sqlite_blob__", base::Base64Encode(bytes));
  base::ListValue values;
  values.Append(7);
  values.Append(std::string("hello\0world", 11));
  values.Append(std::move(blob));
  values.Append(1788423576491.0);
  values.Append(base::Value());
  auto run = Statement("run", insert, base::Value(std::move(values)));
  ASSERT_TRUE(run->success) << run->error;
  EXPECT_EQ(7, run->value.GetDict().FindDouble("lastID"));
  EXPECT_EQ(1, run->value.GetDict().FindInt("changes"));
  ExpectOk(Statement("finalize", insert));
  ExpectOk(Call("close"));
  bridge_ = std::make_unique<XenonSqliteBridge>();
  Open(file.AsUTF8Unsafe());
  const int query = Prepare("SELECT label AS title, payload, expires, optional "
                            "FROM records WHERE id = 7");
  auto result = Statement("get", query);
  ASSERT_TRUE(result->success) << result->error;
  const auto& row = result->value.GetDict();
  EXPECT_EQ(std::string("hello\0world", 11), *row.FindString("title"));
  EXPECT_EQ(1788423576491.0, row.FindDouble("expires"));
  EXPECT_TRUE(row.Find("optional")->is_none());
  ASSERT_TRUE(row.FindDict("payload"));
  EXPECT_EQ(base::Base64Encode(bytes),
            *row.FindDict("payload")->FindString("__xenon_sqlite_blob__"));
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(file, &contents));
  EXPECT_TRUE(contents.starts_with("SQLite format 3"));
  EXPECT_FALSE(base::PathExists(file.AddExtensionASCII("json")));
}

TEST_F(XenonSqliteBridgeTest, TransactionsCommitAndRollbackOnDocumentDestruction) {
  const auto file = temp_.GetPath().AppendASCII("transaction.db");
  Open(file.AsUTF8Unsafe());
  Exec("CREATE TABLE records(value INTEGER); BEGIN; INSERT INTO records VALUES(1)");
  bridge_ = std::make_unique<XenonSqliteBridge>();
  Open(file.AsUTF8Unsafe());
  int query = Prepare("SELECT count(*) AS count FROM records");
  auto count = Statement("get", query);
  ASSERT_TRUE(count->success) << count->error;
  EXPECT_EQ(0, count->value.GetDict().FindDouble("count"));
  ExpectOk(Statement("finalize", query));
  Exec("BEGIN; INSERT INTO records VALUES(2); COMMIT");
  bridge_ = std::make_unique<XenonSqliteBridge>();
  Open(file.AsUTF8Unsafe());
  query = Prepare("SELECT value FROM records");
  auto value = Statement("get", query);
  ASSERT_TRUE(value->success) << value->error;
  EXPECT_EQ(2, value->value.GetDict().FindDouble("value"));
}

TEST_F(XenonSqliteBridgeTest, NamedBindingsCursorResetAndEmptyBindings) {
  Open();
  Exec("CREATE TABLE records(value INTEGER); INSERT INTO records VALUES(1),(2),(3)");
  const int query = Prepare("SELECT value FROM records WHERE value > $minimum ORDER BY value");
  base::DictValue params;
  params.Set("$minimum", 1);
  ExpectOk(Statement("bind", query, base::Value(std::move(params))));
  auto first = Statement("get", query);
  ASSERT_TRUE(first->success);
  EXPECT_EQ(2, first->value.GetDict().FindDouble("value"));
  auto second = Statement("get", query);
  ASSERT_TRUE(second->success);
  EXPECT_EQ(3, second->value.GetDict().FindDouble("value"));
  EXPECT_TRUE(Statement("get", query)->value.is_none());
  EXPECT_TRUE(Statement("get", query)->value.is_none());
  ExpectOk(Statement("reset", query));
  ExpectOk(Statement("bind", query, base::Value(base::ListValue())));
  auto reset = Statement("get", query);
  ASSERT_TRUE(reset->success);
  EXPECT_EQ(2, reset->value.GetDict().FindDouble("value"));
  auto all = Statement("all", query);
  ASSERT_TRUE(all->success);
  EXPECT_EQ(2u, all->value.GetList().size());
}

TEST_F(XenonSqliteBridgeTest, ReportsSqlConstraintBindingAndCloseErrors) {
  Open();
  Exec("CREATE TABLE records(value INTEGER UNIQUE)");
  base::DictValue bad_sql;
  bad_sql.Set("sql", "SELECT FROM invalid");
  auto invalid = Call("prepare", std::move(bad_sql));
  EXPECT_FALSE(invalid->success);
  EXPECT_TRUE(invalid->error.starts_with("SQLITE_ERROR:"));
  const int insert = Prepare("INSERT INTO records VALUES(?)");
  base::ListValue params;
  params.Append(1);
  ExpectOk(Statement("run", insert, base::Value(params.Clone())));
  auto duplicate = Statement("run", insert, base::Value(params.Clone()));
  EXPECT_FALSE(duplicate->success);
  EXPECT_TRUE(duplicate->error.starts_with("SQLITE_CONSTRAINT:"));
  params.Append(2);
  auto range = Statement("bind", insert, base::Value(std::move(params)));
  EXPECT_FALSE(range->success);
  EXPECT_TRUE(range->error.starts_with("SQLITE_RANGE:"));
  auto busy = Call("close");
  EXPECT_FALSE(busy->success);
  EXPECT_TRUE(busy->error.starts_with("SQLITE_BUSY:"));
  ExpectOk(Statement("reset", insert));
  ExpectOk(Statement("finalize", insert));
  auto finalized = Statement("run", insert);
  EXPECT_FALSE(finalized->success);
  EXPECT_TRUE(finalized->error.starts_with("SQLITE_MISUSE:"));
  ExpectOk(Call("close"));
  EXPECT_FALSE(Call("close")->success);
}

TEST_F(XenonSqliteBridgeTest, FinalizeDoesNotRepeatReportedStepError) {
  Open();
  Exec("CREATE TABLE records(value INTEGER UNIQUE); INSERT INTO records VALUES(1)");
  const int insert = Prepare("INSERT INTO records VALUES(1)");
  auto duplicate = Statement("run", insert);
  ASSERT_FALSE(duplicate->success);
  EXPECT_TRUE(duplicate->error.starts_with("SQLITE_CONSTRAINT:"));
  ExpectOk(Statement("finalize", insert));
  ExpectOk(Call("close"));
}

TEST_F(XenonSqliteBridgeTest, UndefinedParametersAreSkippedButNullIsBound) {
  Open();
  const int constant = Prepare("SELECT 1 AS value");
  base::DictValue undefined;
  undefined.Set("__xenon_sqlite_undefined__", true);
  base::ListValue params;
  params.Append(undefined.Clone());
  ExpectOk(Statement("bind", constant, base::Value(params.Clone())));
  auto row = Statement("get", constant);
  ASSERT_TRUE(row->success);
  EXPECT_EQ(1, row->value.GetDict().FindDouble("value"));
  const int query = Prepare("SELECT ? AS first, ? AS second");
  params.Append(2);
  row = Statement("get", query, base::Value(std::move(params)));
  ASSERT_TRUE(row->success);
  EXPECT_TRUE(row->value.GetDict().Find("first")->is_none());
  EXPECT_EQ(2, row->value.GetDict().FindDouble("second"));
  base::ListValue null_params;
  null_params.Append(base::Value());
  auto invalid = Statement("bind", constant, base::Value(std::move(null_params)));
  EXPECT_FALSE(invalid->success);
  EXPECT_TRUE(invalid->error.starts_with("SQLITE_RANGE:"));
}

TEST_F(XenonSqliteBridgeTest, ReadOnlyOpenAndMissingParentReturnRealErrors) {
  base::DictValue missing;
  missing.Set("path", temp_.GetPath().AppendASCII("missing")
                          .AppendASCII("records.db").AsUTF8Unsafe());
  auto unavailable = Call("open", std::move(missing));
  EXPECT_FALSE(unavailable->success);
  EXPECT_TRUE(unavailable->error.starts_with("SQLITE_CANTOPEN:"));
  const auto file = temp_.GetPath().AppendASCII("readonly.db");
  Open(file.AsUTF8Unsafe());
  Exec("CREATE TABLE records(value INTEGER)");
  ExpectOk(Call("close"));
  Open(file.AsUTF8Unsafe(), 1);
  const int insert = Prepare("INSERT INTO records VALUES(1)");
  auto denied = Statement("run", insert);
  EXPECT_FALSE(denied->success);
  EXPECT_TRUE(denied->error.starts_with("SQLITE_READONLY:"));
}

TEST_F(XenonSqliteBridgeTest, HandlesAreScopedToDocumentAndDatabase) {
  Open();
  const int statement = Prepare("SELECT 42 AS value");
  Open();
  auto wrong_database = Statement("get", statement);
  EXPECT_FALSE(wrong_database->success);
  EXPECT_TRUE(wrong_database->error.starts_with("SQLITE_MISUSE:"));
  bridge_ = std::make_unique<XenonSqliteBridge>();
  auto wrong_document = Statement("get", statement);
  EXPECT_FALSE(wrong_document->success);
  EXPECT_TRUE(wrong_document->error.starts_with("SQLITE_MISUSE:"));
}

}  // namespace
}  // namespace xenon::ipc

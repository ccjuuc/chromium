// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_sqlite_bridge.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/string_number_conversions.h"
#include "third_party/sqlite/sqlite3.h"

namespace xenon::ipc {
namespace {

constexpr char kBlobKey[] = "__xenon_sqlite_blob__";

mojom::IpcResultPtr Success(base::Value value = base::Value()) {
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}

mojom::IpcResultPtr Failure(int code, const char* message) {
  static constexpr auto kNames = std::to_array<const char*>({
      "SQLITE_OK", "SQLITE_ERROR", "SQLITE_INTERNAL", "SQLITE_PERM",
      "SQLITE_ABORT", "SQLITE_BUSY", "SQLITE_LOCKED", "SQLITE_NOMEM",
      "SQLITE_READONLY", "SQLITE_INTERRUPT", "SQLITE_IOERR", "SQLITE_CORRUPT",
      "SQLITE_NOTFOUND", "SQLITE_FULL", "SQLITE_CANTOPEN", "SQLITE_PROTOCOL",
      "SQLITE_EMPTY", "SQLITE_SCHEMA", "SQLITE_TOOBIG", "SQLITE_CONSTRAINT",
      "SQLITE_MISMATCH", "SQLITE_MISUSE", "SQLITE_NOLFS", "SQLITE_AUTH",
      "SQLITE_FORMAT", "SQLITE_RANGE", "SQLITE_NOTADB"});
  const size_t primary = static_cast<unsigned>(code) & 0xff;
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = std::string(primary < kNames.size() ? kNames[primary]
                                                    : "SQLITE_ERROR") +
                  ": " + message;
  return result;
}

struct CloseDatabase {
  void operator()(sqlite3* db) const { sqlite3_close_v2(db); }
};
struct FinalizeStatement {
  void operator()(sqlite3_stmt* stmt) const { sqlite3_finalize(stmt); }
};
using ScopedStatement = std::unique_ptr<sqlite3_stmt, FinalizeStatement>;

int BindValue(sqlite3_stmt* stmt, int index, const base::Value& value) {
  // node-sqlite3 skips undefined parameters, including the optional undefined
  // argument in prepare(sql, undefined, callback). Null is a real SQL value.
  if (value.is_dict() &&
      value.GetDict().FindBool("__xenon_sqlite_undefined__").value_or(false)) {
    return SQLITE_OK;
  }
  if (index <= 0 || index > sqlite3_bind_parameter_count(stmt)) {
    return SQLITE_RANGE;
  }
  if (value.is_none()) {
    return sqlite3_bind_null(stmt, index);
  }
  if (value.is_bool()) {
    return sqlite3_bind_int(stmt, index, value.GetBool());
  }
  if (value.is_int()) {
    return sqlite3_bind_int(stmt, index, value.GetInt());
  }
  if (value.is_double()) {
    return sqlite3_bind_double(stmt, index, value.GetDouble());
  }
  if (value.is_string()) {
    const std::string& text = value.GetString();
    return sqlite3_bind_text64(stmt, index, text.data(), text.size(),
                               SQLITE_TRANSIENT, SQLITE_UTF8);
  }
  if (value.is_dict()) {
    const std::string* encoded = value.GetDict().FindString(kBlobKey);
    std::string bytes;
    if (encoded && base::Base64Decode(*encoded, &bytes)) {
      return sqlite3_bind_blob64(stmt, index, bytes.data(), bytes.size(),
                                 SQLITE_TRANSIENT);
    }
  }
  return SQLITE_MISMATCH;
}

int BindParameters(sqlite3_stmt* stmt, const base::Value* params,
                   bool* rebound) {
  *rebound = false;
  if (!params || (params->is_list() && params->GetList().empty()) ||
      (params->is_dict() && params->GetDict().empty())) {
    return SQLITE_OK;
  }
  if (!params->is_list() && !params->is_dict()) {
    return SQLITE_MISUSE;
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  *rebound = true;
  if (params->is_list()) {
    int index = 1;
    for (const auto& value : params->GetList()) {
      const int code = BindValue(stmt, index++, value);
      if (code != SQLITE_OK) {
        return code;
      }
    }
  } else {
    for (const auto [key, value] : params->GetDict()) {
      int index = 0;
      if (!base::StringToInt(key, &index)) {
        index = sqlite3_bind_parameter_index(stmt, key.c_str());
      }
      const int code = BindValue(stmt, index, value);
      if (code != SQLITE_OK) {
        return code;
      }
    }
  }
  return SQLITE_OK;
}

base::Value ReadRow(sqlite3_stmt* stmt) {
  base::DictValue row;
  for (int column = 0; column < sqlite3_column_count(stmt); ++column) {
    base::Value value;
    switch (sqlite3_column_type(stmt, column)) {
      case SQLITE_INTEGER:
      case SQLITE_FLOAT:
        value = base::Value(sqlite3_column_double(stmt, column));
        break;
      case SQLITE_TEXT: {
        const char* text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
        value = base::Value(std::string(text, sqlite3_column_bytes(stmt, column)));
        break;
      }
      case SQLITE_BLOB: {
        const auto* data =
            static_cast<const uint8_t*>(sqlite3_column_blob(stmt, column));
        const size_t size = sqlite3_column_bytes(stmt, column);
        // SQLite owns exactly `size` readable bytes until the next step/reset.
        const auto bytes = UNSAFE_BUFFERS(base::span(data, size));
        base::DictValue blob;
        blob.Set(kBlobKey, base::Base64Encode(bytes));
        value = base::Value(std::move(blob));
        break;
      }
      default:
        break;
    }
    row.Set(sqlite3_column_name(stmt, column), std::move(value));
  }
  return base::Value(std::move(row));
}

}  // namespace

struct XenonSqliteBridge::Database {
  struct Statement {
    ScopedStatement handle;
    bool exhausted = false;
  };
  // Reverse member destruction finalizes statements before closing the DB.
  std::unique_ptr<sqlite3, CloseDatabase> handle;
  std::map<int, Statement> statements;
};

XenonSqliteBridge::XenonSqliteBridge() = default;
XenonSqliteBridge::~XenonSqliteBridge() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

mojom::IpcResultPtr XenonSqliteBridge::Call(base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!arguments.is_list() || arguments.GetList().size() != 1 ||
      !arguments.GetList()[0].is_dict()) {
    return Failure(SQLITE_MISUSE, "Invalid SQLite request");
  }
  const base::DictValue& request = arguments.GetList()[0].GetDict();
  const std::string* operation = request.FindString("operation");
  if (!operation) {
    return Failure(SQLITE_MISUSE, "Missing SQLite operation");
  }
  if (*operation == "open") {
    const std::string* path = request.FindString("path");
    if (!path || path->find('\0') != std::string::npos) {
      return Failure(SQLITE_MISUSE, "Invalid database path");
    }
    // Chromium's SQLite build disables implicit initialization. Never call
    // sqlite3_shutdown(): the library is shared with browser databases.
    const int initialized = sqlite3_initialize();
    if (initialized != SQLITE_OK) {
      return Failure(initialized, sqlite3_errstr(initialized));
    }
    auto database = std::make_unique<Database>();
    sqlite3* raw = nullptr;
    const int flags = request.FindInt("mode").value_or(
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);
    const int code = sqlite3_open_v2(path->c_str(), &raw, flags, nullptr);
    database->handle.reset(raw);
    if (code != SQLITE_OK) {
      return Failure(code, raw ? sqlite3_errmsg(raw) : sqlite3_errstr(code));
    }
    sqlite3_busy_timeout(raw, 1000);
    const int id = next_database_id_++;
    databases_.emplace(id, std::move(database));
    return Success(base::Value(id));
  }

  auto found = databases_.find(request.FindInt("databaseId").value_or(0));
  if (found == databases_.end()) {
    return Failure(SQLITE_MISUSE, "Database is closed or belongs to another document");
  }
  Database& database = *found->second;
  sqlite3* db = database.handle.get();
  if (*operation == "close") {
    const int code = sqlite3_close(db);
    if (code != SQLITE_OK) {
      return Failure(code, sqlite3_errmsg(db));
    }
    database.handle.release();
    databases_.erase(found);
    return Success();
  }
  if (*operation == "configure") {
    const auto timeout = request.FindInt("busyTimeout");
    if (!timeout || *timeout < 0) {
      return Failure(SQLITE_MISUSE, "Invalid busyTimeout");
    }
    sqlite3_busy_timeout(db, *timeout);
    return Success();
  }
  if (*operation == "exec" || *operation == "prepare") {
    const std::string* sql = request.FindString("sql");
    if (!sql || sql->find('\0') != std::string::npos) {
      return Failure(SQLITE_MISUSE, "Invalid SQL text");
    }
    if (*operation == "exec") {
      const int code = sqlite3_exec(db, sql->c_str(), nullptr, nullptr, nullptr);
      return code == SQLITE_OK ? Success() : Failure(code, sqlite3_errmsg(db));
    }
    sqlite3_stmt* raw = nullptr;
    const int code = sqlite3_prepare_v2(db, sql->c_str(), -1, &raw, nullptr);
    ScopedStatement statement(raw);
    if (code != SQLITE_OK || !raw) {
      return Failure(code == SQLITE_OK ? SQLITE_ERROR : code,
                       code == SQLITE_OK ? "SQL contains no statement" : sqlite3_errmsg(db));
    }
    const int id = next_statement_id_++;
    database.statements.emplace(id, Database::Statement{std::move(statement)});
    return Success(base::Value(id));
  }

  const int statement_id = request.FindInt("statementId").value_or(0);
  auto statement_it = database.statements.find(statement_id);
  if (statement_it == database.statements.end()) {
    return Failure(SQLITE_MISUSE, "Statement is finalized or belongs to another database");
  }
  auto& state = statement_it->second;
  sqlite3_stmt* stmt = state.handle.get();
  if (*operation == "finalize") {
    // finalize's result repeats the last step error, already reported to the
    // operation callback. node-sqlite3 does not report that error a second time.
    database.statements.erase(statement_it);
    return Success();
  }
  if (*operation == "reset") {
    sqlite3_reset(stmt);
    state.exhausted = false;
    return Success();
  }
  if (*operation != "bind" && *operation != "run" &&
      *operation != "get" && *operation != "all") {
    return Failure(SQLITE_MISUSE, "Unsupported SQLite operation");
  }
  bool rebound = false;
  int code = BindParameters(stmt, request.Find("params"), &rebound);
  if (rebound) {
    state.exhausted = false;
  }
  if (code != SQLITE_OK) {
    return Failure(code, sqlite3_errstr(code));
  }
  if (*operation == "bind") {
    return Success();
  }
  if (*operation == "run" || *operation == "all") {
    sqlite3_reset(stmt);
    state.exhausted = false;
  }
  if (*operation == "get" && state.exhausted) {
    return Success();
  }
  base::ListValue rows;
  while ((code = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (*operation == "get") {
      return Success(ReadRow(stmt));
    }
    if (*operation == "run") {
      break;
    }
    rows.Append(ReadRow(stmt));
  }
  state.exhausted = code == SQLITE_DONE;
  if (code != SQLITE_DONE && code != SQLITE_ROW) {
    return Failure(code, sqlite3_errmsg(db));
  }
  if (*operation == "run") {
    base::DictValue info;
    info.Set("lastID", static_cast<double>(sqlite3_last_insert_rowid(db)));
    info.Set("changes", sqlite3_changes(db));
    return Success(base::Value(std::move(info)));
  }
  return *operation == "all" ? Success(base::Value(std::move(rows))) : Success();
}

}  // namespace xenon::ipc

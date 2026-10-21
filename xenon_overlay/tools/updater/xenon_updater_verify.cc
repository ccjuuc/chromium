// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Uses the same parser and CRX verifier as Chromium Updater, without executing
// downloaded code or registering a service.
#include <iostream>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "components/crx_file/crx_verifier.h"
#include "components/update_client/protocol_parser_json.h"

int main(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
  const auto& command = *base::CommandLine::ForCurrentProcess();
  base::DictValue result;
  bool performed_check = false;
  if (command.HasSwitch("response")) {
    std::string response;
    update_client::ProtocolParserJSON parser;
    if (!base::ReadFileToString(command.GetSwitchValuePath("response"),
                                &response) ||
        !parser.Parse(response) || !parser.errors().empty()) {
      std::cerr << "Omaha response rejected: " << parser.errors() << '\n';
      return 1;
    }
    base::ListValue apps;
    for (const auto& app : parser.results().apps) {
      base::DictValue entry;
      entry.Set("appid", app.app_id);
      entry.Set("status", app.status);
      if (app.nextversion.IsValid()) {
        entry.Set("version", app.nextversion.GetString());
      }
      base::ListValue pipelines;
      for (const auto& pipeline : app.pipelines) {
        base::ListValue operations;
        for (const auto& operation : pipeline.operations) {
          operations.Append(operation.type);
        }
        pipelines.Append(std::move(operations));
      }
      entry.Set("pipelines", std::move(pipelines));
      apps.Append(std::move(entry));
    }
    result.Set("apps", std::move(apps));
    performed_check = true;
  }
  if (command.HasSwitch("crx")) {
    std::vector<uint8_t> pin;
    std::vector<uint8_t> file_hash;
    if (!base::HexStringToBytes(command.GetSwitchValueASCII("key-sha256"),
                                &pin) ||
        pin.size() != 32 ||
        !base::HexStringToBytes(command.GetSwitchValueASCII("file-sha256"),
                                &file_hash) ||
        file_hash.size() != 32) {
      std::cerr << "Both 32-byte key and file SHA256 hashes are required\n";
      return 1;
    }
    const auto status = crx_file::Verify(command.GetSwitchValuePath("crx"),
                                         crx_file::VerifierFormat::CRX3, {pin},
                                         file_hash, nullptr, nullptr, nullptr);
    if (status != crx_file::VerifierResult::OK_FULL) {
      std::cerr << "CRX rejected: " << static_cast<int>(status) << '\n';
      return 1;
    }
    result.Set("crx_signature_and_hash_verified", true);
    performed_check = true;
  }
  if (!performed_check) {
    std::cerr << "Use --response=FILE and/or --crx=FILE "
                 "--key-sha256=HEX --file-sha256=HEX\n";
    return 1;
  }
  std::string json;
  if (!base::JSONWriter::Write(result, &json)) {
    return 1;
  }
  std::cout << json << '\n';
  return 0;
}

// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_bridge.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "base/numerics/safe_conversions.h"
#include "third_party/zlib/zlib.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaxOutputLength = 64 * 1024 * 1024;

mojom::IpcResultPtr Failure(const std::string& code,
                            const std::string& message) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = code + ": " + message;
  return result;
}

const char* ErrorCode(int code) {
  switch (code) {
    case Z_NEED_DICT:
      return "Z_NEED_DICT";
    case Z_STREAM_ERROR:
      return "Z_STREAM_ERROR";
    case Z_DATA_ERROR:
      return "Z_DATA_ERROR";
    case Z_MEM_ERROR:
      return "Z_MEM_ERROR";
    case Z_BUF_ERROR:
      return "Z_BUF_ERROR";
    default:
      return "Z_VERSION_ERROR";
  }
}
}  // namespace

mojom::IpcResultPtr PerformZlibCall(std::string operation,
                                    base::Value::BlobStorage input,
                                    base::DictValue options) {
  const bool compress = operation == "gzip" || operation == "deflate" ||
                        operation == "deflateRaw";
  const bool decompress = operation == "gunzip" || operation == "inflate" ||
                          operation == "inflateRaw" || operation == "unzip";
  if (!compress && !decompress) {
    return Failure("ERR_NOT_SUPPORTED",
                   "Unsupported zlib operation: " + operation);
  }
  if (input.size() > std::numeric_limits<uInt>::max()) {
    return Failure("ERR_OUT_OF_RANGE", "zlib input is too large");
  }
  for (const auto [key, value] : options) {
    if (key != "level" && key != "windowBits" && key != "memLevel" &&
        key != "strategy" && key != "maxOutputLength" && key != "chunkSize" &&
        key != "flush" && key != "finishFlush") {
      return Failure("ERR_NOT_SUPPORTED", "Unsupported zlib option: " + key);
    }
    if (!value.is_int()) {
      return Failure("ERR_OUT_OF_RANGE", "zlib options must be integers");
    }
  }
  const int level = options.FindInt("level").value_or(Z_DEFAULT_COMPRESSION);
  const int window = options.FindInt("windowBits").value_or(MAX_WBITS);
  const int memory = options.FindInt("memLevel").value_or(8);
  const int strategy = options.FindInt("strategy").value_or(Z_DEFAULT_STRATEGY);
  const int max_output =
      options.FindInt("maxOutputLength").value_or(kMaxOutputLength);
  if (level < -1 || level > 9 || window < 9 || window > 15 || memory < 1 ||
      memory > 9 || strategy < 0 || strategy > 4 || max_output <= 0 ||
      max_output > static_cast<int>(kMaxOutputLength) ||
      options.FindInt("chunkSize").value_or(16384) < 64) {
    return Failure("ERR_OUT_OF_RANGE",
                   "zlib option is outside the supported range");
  }
  if (options.FindInt("flush").value_or(Z_NO_FLUSH) != Z_NO_FLUSH ||
      options.FindInt("finishFlush").value_or(Z_FINISH) != Z_FINISH) {
    return Failure("ERR_NOT_SUPPORTED", "Partial zlib flush is not supported");
  }
  int window_bits = window;
  if (operation == "gzip" || operation == "gunzip") {
    window_bits += 16;
  }
  if (operation == "unzip") {
    window_bits += 32;
  }
  if (operation == "deflateRaw" || operation == "inflateRaw") {
    window_bits = -window;
  }
  z_stream stream = {};
  int status = compress ? deflateInit2(&stream, level, Z_DEFLATED, window_bits,
                                       memory, strategy)
                        : inflateInit2(&stream, window_bits);
  if (status != Z_OK) {
    return Failure(ErrorCode(status), "Cannot initialize zlib");
  }
  stream.next_in = input.data();
  stream.avail_in = base::checked_cast<uInt>(input.size());
  std::array<uint8_t, 16384> chunk;
  base::Value::BlobStorage output;
  bool too_large = false;
  // gunzip/unzip accept concatenated gzip members. Zlib/raw trailing bytes keep
  // the standard one-shot behavior and are not mistaken for another stream.
  const bool gzip_input =
      input.size() >= 2 && input[0] == 0x1f && input[1] == 0x8b;
  do {
    stream.next_out = chunk.data();
    stream.avail_out = chunk.size();
    status = compress ? deflate(&stream, Z_FINISH) : inflate(&stream, Z_FINISH);
    const size_t written = chunk.size() - stream.avail_out;
    if (written > static_cast<size_t>(max_output) - output.size()) {
      too_large = true;
      break;
    }
    output.insert(output.end(), chunk.begin(), chunk.begin() + written);
    if (!compress && status == Z_STREAM_END && gzip_input && stream.avail_in) {
      if (*stream.next_in == 0) {
        break;  // Permitted gzip zero padding.
      }
      status = inflateReset(&stream);
      if (status != Z_OK) {
        break;
      }
      continue;
    }
    if (status == Z_BUF_ERROR && stream.avail_out == 0) {
      status = Z_OK;
    }
  } while (status == Z_OK);
  const std::string message =
      stream.msg ? stream.msg : "Incomplete compressed data";
  if (compress) {
    deflateEnd(&stream);
  } else {
    inflateEnd(&stream);
  }
  if (too_large) {
    return Failure("ERR_BUFFER_TOO_LARGE",
                   "zlib output exceeds maxOutputLength");
  }
  if (status != Z_STREAM_END) {
    return Failure(ErrorCode(status), message);
  }
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = base::Value(std::move(output));
  return result;
}
}  // namespace xenon::ipc

// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_stream.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "third_party/brotli/include/brotli/decode.h"
#include "third_party/brotli/include/brotli/encode.h"
#include "third_party/zlib/zlib.h"
#include "third_party/zstd/src/lib/zstd.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaximumOutput = 64 * 1024 * 1024;
mojom::IpcResultPtr Failure(const std::string& error) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = error;
  return result;
}
const char* ZlibError(int result) {
  switch (result) {
    case Z_NEED_DICT:
      return "Z_NEED_DICT";
    case Z_STREAM_ERROR:
      return "Z_STREAM_ERROR";
    case Z_DATA_ERROR:
      return "Z_DATA_ERROR";
    case Z_MEM_ERROR:
      return "Z_MEM_ERROR";
    default:
      return "Z_BUF_ERROR";
  }
}
bool IntegerOption(const base::DictValue& options,
                   const char* key,
                   int fallback,
                   int minimum,
                   int maximum,
                   int* value,
                   std::string* error) {
  const base::Value* input = options.Find(key);
  if (input && !input->is_int()) {
    *error =
        std::string("ERR_INVALID_ARG_TYPE: ") + key + " must be an integer";
    return false;
  }
  *value = input ? input->GetInt() : fallback;
  if (*value < minimum || *value > maximum) {
    *error = std::string("ERR_OUT_OF_RANGE: invalid ") + key;
    return false;
  }
  return true;
}
}  // namespace

struct ZlibStream::State {
  enum class Format { kZlib, kBrotli, kZstd };
  Format format = Format::kZlib;
  bool compress = false;
  bool initialized = false;
  bool ended = false;
  bool gzip = false;
  bool detect_gzip = false;
  bool padding = false;
  size_t prefix_length = 0;
  std::array<uint8_t, 2> prefix = {};
  z_stream z = {};
  base::Value::BlobStorage dictionary;
  std::unique_ptr<BrotliEncoderState, decltype(&BrotliEncoderDestroyInstance)>
      brotli_encoder{nullptr, &BrotliEncoderDestroyInstance};
  std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>
      brotli_decoder{nullptr, &BrotliDecoderDestroyInstance};
  std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> zstd_encoder{
      nullptr, &ZSTD_freeCCtx};
  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> zstd_decoder{
      nullptr, &ZSTD_freeDCtx};
  ~State() {
    if (initialized && format == Format::kZlib) {
      if (compress) {
        deflateEnd(&z);
      } else {
        inflateEnd(&z);
      }
    }
  }
};

ZlibStream::ZlibStream(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
ZlibStream::~ZlibStream() = default;

scoped_refptr<ZlibStream> ZlibStream::Create(const std::string& operation,
                                             const base::DictValue& options,
                                             std::string* error) {
  auto state = std::make_unique<State>();
  int chunk_size;
  if (!IntegerOption(options, "chunkSize", 16384, 64,
                     std::numeric_limits<int>::max(), &chunk_size, error)) {
    return nullptr;
  }
  for (const auto [key, value] : options) {
    if (key != "level" && key != "windowBits" && key != "memLevel" &&
        key != "strategy" && key != "dictionary" && key != "params" &&
        key != "chunkSize" && key != "maxOutputLength" && key != "flush" &&
        key != "finishFlush") {
      *error = "ERR_NOT_SUPPORTED: Unsupported codec option: " + key;
      return nullptr;
    }
  }
  state->compress = operation == "gzip" || operation == "deflate" ||
                    operation == "deflateRaw" ||
                    operation == "brotliCompress" ||
                    operation == "zstdCompress";
  if (operation == "brotliCompress" || operation == "brotliDecompress") {
    state->format = State::Format::kBrotli;
    if (state->compress) {
      state->brotli_encoder.reset(
          BrotliEncoderCreateInstance(nullptr, nullptr, nullptr));
    } else {
      state->brotli_decoder.reset(
          BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
    }
    if (!(state->compress ? !!state->brotli_encoder
                          : !!state->brotli_decoder)) {
      *error = "ERR_MEMORY_ALLOCATION_FAILED: Cannot initialize Brotli";
      return nullptr;
    }
  } else if (operation == "zstdCompress" || operation == "zstdDecompress") {
    state->format = State::Format::kZstd;
    if (state->compress) {
      state->zstd_encoder.reset(ZSTD_createCCtx());
    } else {
      state->zstd_decoder.reset(ZSTD_createDCtx());
    }
    if (!(state->compress ? !!state->zstd_encoder : !!state->zstd_decoder)) {
      *error = "ERR_MEMORY_ALLOCATION_FAILED: Cannot initialize Zstd";
      return nullptr;
    }
    if (!state->compress) {
      ZSTD_DCtx_setParameter(state->zstd_decoder.get(), ZSTD_d_windowLogMax,
                             26);
    }
  } else {
    if (!state->compress && operation != "gunzip" && operation != "inflate" &&
        operation != "inflateRaw" && operation != "unzip") {
      *error = "ERR_NOT_SUPPORTED: Unknown compression format: " + operation;
      return nullptr;
    }
    int level, window, memory, strategy;
    const bool automatic_window =
        operation == "inflate" || operation == "gunzip" || operation == "unzip";
    if (!IntegerOption(options, "level", -1, -1, 9, &level, error) ||
        !IntegerOption(options, "windowBits", 15, automatic_window ? 0 : 8, 15,
                       &window, error) ||
        !IntegerOption(options, "memLevel", 8, 1, 9, &memory, error) ||
        !IntegerOption(options, "strategy", 0, 0, 4, &strategy, error)) {
      return nullptr;
    }
    if (window > 0 && window < 8) {
      *error = "ERR_OUT_OF_RANGE: Invalid windowBits";
      return nullptr;
    }
    // zlib upgrades an 8-bit compression window to 9 bits; raw mode must use
    // the same effective window rather than rejecting the public Node option.
    if (state->compress && window == 8) {
      window = 9;
    }
    state->gzip = operation == "gunzip";
    state->detect_gzip = operation == "unzip";
    if (operation == "gzip" || operation == "gunzip") {
      window += 16;
    } else if (operation == "unzip") {
      window += 32;
    } else if (operation == "deflateRaw" || operation == "inflateRaw") {
      window = -window;
    }
    const int status = state->compress
                           ? deflateInit2(&state->z, level, Z_DEFLATED, window,
                                          memory, strategy)
                           : inflateInit2(&state->z, window);
    if (status != Z_OK) {
      *error = std::string(ZlibError(status)) + ": Cannot initialize codec";
      return nullptr;
    }
    state->initialized = true;
    if (const auto* dictionary = options.Find("dictionary")) {
      if (!dictionary->is_blob() ||
          dictionary->GetBlob().size() > kMaximumOutput) {
        *error =
            "ERR_INVALID_ARG_TYPE: dictionary must be a bounded binary buffer";
        return nullptr;
      }
      state->dictionary = dictionary->GetBlob();
      if ((state->compress && operation != "gzip") ||
          operation == "inflateRaw") {
        int result =
            state->compress
                ? deflateSetDictionary(
                      &state->z, state->dictionary.data(),
                      base::checked_cast<uInt>(state->dictionary.size()))
                : inflateSetDictionary(
                      &state->z, state->dictionary.data(),
                      base::checked_cast<uInt>(state->dictionary.size()));
        if (result != Z_OK) {
          *error = std::string(ZlibError(result)) +
                   ": Invalid compression dictionary";
          return nullptr;
        }
      }
    }
  }
  if (state->format != State::Format::kZlib) {
    for (const char* key :
         {"dictionary", "level", "windowBits", "memLevel", "strategy"}) {
      if (options.contains(key)) {
        *error =
            std::string("ERR_NOT_SUPPORTED: Use codec params instead of ") +
            key;
        return nullptr;
      }
    }
    const auto* value = options.Find("params");
    if (value && !value->is_dict()) {
      *error = "ERR_INVALID_ARG_TYPE: Codec params must be an object";
      return nullptr;
    }
    if (value) {
      for (const auto [key, parameter] : value->GetDict()) {
        int index;
        if (!base::StringToInt(key, &index) || !parameter.is_int()) {
          *error =
              "ERR_INVALID_ARG_TYPE: Codec parameters must have integer keys "
              "and values";
          return nullptr;
        }
        const int setting = parameter.GetInt();
        if (state->format == State::Format::kBrotli) {
          // Standard Brotli windows are at most 24 bits. Large-window
          // extensions would defeat bounded decoder memory and are not silently
          // enabled.
          if ((!state->compress && index == BROTLI_DECODER_PARAM_LARGE_WINDOW &&
               setting) ||
              (state->compress && index == BROTLI_PARAM_LARGE_WINDOW &&
               setting) ||
              (state->compress && index == BROTLI_PARAM_LGWIN &&
               setting > 24)) {
            *error =
                "ERR_NOT_SUPPORTED: Brotli large-window mode is not supported";
            return nullptr;
          }
          const auto ok =
              state->compress
                  ? BrotliEncoderSetParameter(
                        state->brotli_encoder.get(),
                        static_cast<BrotliEncoderParameter>(index), setting)
                  : BrotliDecoderSetParameter(
                        state->brotli_decoder.get(),
                        static_cast<BrotliDecoderParameter>(index), setting);
          if (!ok) {
            *error = "ERR_OUT_OF_RANGE: Invalid Brotli parameter " + key;
            return nullptr;
          }
        } else {
          // Experimental parameters include stable caller-buffer modes. They
          // cannot be enabled when each request owns a different bounded
          // buffer.
          const bool public_parameter =
              state->compress
                  ? ((index >= 100 && index <= 107) || index == 130 ||
                     (index >= 160 && index <= 164) ||
                     (index >= 200 && index <= 202) ||
                     (index >= 400 && index <= 402))
                  : index == ZSTD_d_windowLogMax;
          if (!public_parameter) {
            *error = "ERR_NOT_SUPPORTED: Unsupported Zstd parameter " + key;
            return nullptr;
          }
          if ((state->compress && index == ZSTD_c_nbWorkers && setting != 0) ||
              (state->compress && index == ZSTD_c_windowLog && setting > 26) ||
              (state->compress &&
               (index == ZSTD_c_hashLog || index == ZSTD_c_chainLog ||
                index == ZSTD_c_ldmHashLog) &&
               setting > 26) ||
              (!state->compress && index == ZSTD_d_windowLogMax &&
               setting > 26)) {
            *error =
                "ERR_NOT_SUPPORTED: Zstd workers and windows above 64 MiB are "
                "not supported";
            return nullptr;
          }
          const int bounded_setting =
              !state->compress && index == ZSTD_d_windowLogMax && setting == 0
                  ? 26
                  : setting;
          const size_t result =
              state->compress
                  ? ZSTD_CCtx_setParameter(state->zstd_encoder.get(),
                                           static_cast<ZSTD_cParameter>(index),
                                           bounded_setting)
                  : ZSTD_DCtx_setParameter(state->zstd_decoder.get(),
                                           static_cast<ZSTD_dParameter>(index),
                                           bounded_setting);
          if (ZSTD_isError(result)) {
            *error = "ERR_OUT_OF_RANGE: Invalid Zstd parameter " + key;
            return nullptr;
          }
        }
      }
    }
  } else if (options.contains("params")) {
    *error = "ERR_NOT_SUPPORTED: zlib does not support codec params";
    return nullptr;
  }
  return base::WrapRefCounted(new ZlibStream(std::move(state)));
}

bool ZlibStream::TryBegin() {
  bool expected = false;
  return !cancelled_.load() && busy_.compare_exchange_strong(expected, true);
}
void ZlibStream::Cancel() {
  cancelled_.store(true);
}

mojom::IpcResultPtr ZlibStream::Process(base::Value::BlobStorage input,
                                        int flush,
                                        int output_size) {
  struct ReleaseBusy {
    std::atomic<bool>& busy;
    ~ReleaseBusy() { busy.store(false); }
  } release{busy_};
  if (cancelled_.load()) {
    return Failure("ERR_STREAM_DESTROYED: Codec was closed");
  }
  if (input.size() > kMaximumChunk || output_size < 64 ||
      output_size > static_cast<int>(kMaximumChunk)) {
    return Failure(
        "ERR_OUT_OF_RANGE: Compression chunks must be bounded to 64 KiB");
  }
  auto& state = *state_;
  base::Value::BlobStorage output(output_size);
  size_t consumed = 0, written = 0;
  bool done = false;
  const bool finish =
      flush == (state.format == State::Format::kZlib ? Z_FINISH : 2);
  if (flush < 0 || flush > (state.format == State::Format::kZlib     ? Z_BLOCK
                            : state.format == State::Format::kBrotli ? 3
                                                                     : 2)) {
    return Failure("ERR_OUT_OF_RANGE: Invalid compression flush operation");
  }
  if (state.compress && state.ended) {
    if (!input.empty()) {
      return Failure("ERR_STREAM_WRITE_AFTER_END: Codec has finished");
    }
    done = true;
  } else if (state.format == State::Format::kZlib) {
    if (state.detect_gzip && state.prefix_length < 2) {
      for (uint8_t byte : input) {
        state.prefix[state.prefix_length++] = byte;
        if (state.prefix_length == 2) {
          break;
        }
      }
      state.gzip = state.prefix_length == 2 && state.prefix[0] == 0x1f &&
                   state.prefix[1] == 0x8b;
    }
    if (state.ended && !state.compress) {
      if (!state.gzip || state.padding || input.empty() || input.front() == 0) {
        state.padding = state.padding || !input.empty();
        consumed = input.size();
        done = true;
      } else {
        const int reset = inflateReset(&state.z);
        if (reset != Z_OK) {
          return Failure(std::string(ZlibError(reset)) +
                         ": Cannot reset gzip member");
        }
        state.ended = false;
      }
    }
    if (!done) {
      state.z.next_in = input.data();
      state.z.avail_in = base::checked_cast<uInt>(input.size());
      state.z.next_out = output.data();
      state.z.avail_out = base::checked_cast<uInt>(output.size());
      int status =
          state.compress ? deflate(&state.z, flush) : inflate(&state.z, flush);
      if (status == Z_NEED_DICT && !state.dictionary.empty()) {
        status = inflateSetDictionary(
            &state.z, state.dictionary.data(),
            base::checked_cast<uInt>(state.dictionary.size()));
        if (status == Z_OK) {
          status = inflate(&state.z, flush);
        }
      }
      consumed = input.size() - state.z.avail_in;
      written = output.size() - state.z.avail_out;
      // The C library does not retain references to caller input/output.
      state.z.next_in = nullptr;
      state.z.next_out = nullptr;
      state.ended = status == Z_STREAM_END;
      if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
        return Failure(std::string(ZlibError(status)) + ": " +
                       (state.z.msg ? state.z.msg : "Compression failed"));
      }
      if (state.ended && (!state.gzip || state.compress)) {
        consumed = input.size();
      }
      done = consumed == input.size() && (state.z.avail_out > 0 || state.ended);
      if (finish && done && !state.ended) {
        return Failure("Z_BUF_ERROR: Unexpected end of compressed data");
      }
    }
  } else if (state.format == State::Format::kBrotli) {
    size_t available_in = input.size(), available_out = output.size();
    const uint8_t* next_in = input.data();
    uint8_t* next_out = output.data();
    if (state.compress) {
      if (!BrotliEncoderCompressStream(
              state.brotli_encoder.get(),
              static_cast<BrotliEncoderOperation>(flush), &available_in,
              &next_in, &available_out, &next_out, nullptr)) {
        return Failure(
            "ERR_BROTLI_COMPRESSION_FAILED: Brotli compression failed");
      }
      state.ended = BrotliEncoderIsFinished(state.brotli_encoder.get());
      done = !available_in &&
             !BrotliEncoderHasMoreOutput(state.brotli_encoder.get()) &&
             (!finish || state.ended);
    } else if (state.ended) {
      available_in = 0;
      done = true;
    } else {
      const auto status = BrotliDecoderDecompressStream(
          state.brotli_decoder.get(), &available_in, &next_in, &available_out,
          &next_out, nullptr);
      if (status == BROTLI_DECODER_RESULT_ERROR) {
        return Failure(std::string("ERR_BROTLI_DECODER_ERROR: ") +
                       BrotliDecoderErrorString(BrotliDecoderGetErrorCode(
                           state.brotli_decoder.get())));
      }
      state.ended = status == BROTLI_DECODER_RESULT_SUCCESS;
      if (state.ended) {
        available_in = 0;
      }
      // A flush boundary can request more input while the ring buffer still
      // has decoded output. Drain that output before completing this write.
      done = !available_in &&
             status != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT &&
             !BrotliDecoderHasMoreOutput(state.brotli_decoder.get());
      if (finish && done && !state.ended) {
        return Failure("Z_BUF_ERROR: Unexpected end of Brotli data");
      }
    }
    consumed = input.size() - available_in;
    written = output.size() - available_out;
  } else {
    ZSTD_inBuffer source{input.data(), input.size(), 0};
    ZSTD_outBuffer destination{output.data(), output.size(), 0};
    if (!state.compress && state.ended && input.empty()) {
      done = true;
    } else {
      const size_t status =
          state.compress ? ZSTD_compressStream2(
                               state.zstd_encoder.get(), &destination, &source,
                               static_cast<ZSTD_EndDirective>(flush))
                         : ZSTD_decompressStream(state.zstd_decoder.get(),
                                                 &destination, &source);
      if (ZSTD_isError(status)) {
        return Failure(std::string("ERR_ZSTD_GENERIC: ") +
                       ZSTD_getErrorName(status));
      }
      state.ended = status == 0 && (!state.compress || finish);
      done = source.pos == source.size &&
             (state.compress
                  ? (flush == 0 || status == 0)
                  : (state.ended || destination.pos < destination.size));
      if (!state.compress && finish && done && !state.ended) {
        return Failure("Z_BUF_ERROR: Unexpected end of Zstd data");
      }
    }
    consumed = source.pos;
    written = destination.pos;
  }
  output.resize(written);
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value =
      base::Value(base::DictValue()
                      .Set("consumed", base::checked_cast<int>(consumed))
                      .Set("output", std::move(output))
                      .Set("done", done)
                      .Set("finished", state.ended));
  return result;
}

mojom::IpcResultPtr PerformStreamingZlibCall(std::string operation,
                                             base::Value::BlobStorage input,
                                             base::DictValue options) {
  std::string error;
  int maximum;
  if (!IntegerOption(options, "maxOutputLength", kMaximumOutput, 1,
                     kMaximumOutput, &maximum, &error)) {
    return Failure(error);
  }
  auto stream = ZlibStream::Create(operation, options, &error);
  if (!stream) {
    return Failure(error);
  }
  const bool extended =
      operation.starts_with("brotli") || operation.starts_with("zstd");
  const int finish =
      options.FindInt("finishFlush").value_or(extended ? 2 : Z_FINISH);
  base::Value::BlobStorage output;
  size_t offset = 0;
  for (;;) {
    const size_t length =
        std::min(input.size() - offset, ZlibStream::kMaximumChunk);
    auto slice = base::span(input).subspan(offset, length);
    const bool last = offset + length == input.size();
    if (!stream->TryBegin()) {
      return Failure("ERR_STREAM_DESTROYED: Codec closed");
    }
    auto step = stream->Process({slice.begin(), slice.end()}, last ? finish : 0,
                                ZlibStream::kMaximumChunk);
    if (!step->success) {
      return step;
    }
    const auto& value = step->value.GetDict();
    const auto& bytes = *value.FindBlob("output");
    if (bytes.size() > static_cast<size_t>(maximum) - output.size()) {
      return Failure(
          "ERR_BUFFER_TOO_LARGE: Compression output exceeds maxOutputLength");
    }
    output.insert(output.end(), bytes.begin(), bytes.end());
    const int consumed = *value.FindInt("consumed");
    offset += consumed;
    if (last && value.FindBool("done").value_or(false)) {
      break;
    }
    if (!consumed && bytes.empty() && !value.FindBool("done").value_or(false)) {
      return Failure("Z_BUF_ERROR: Codec made no progress");
    }
  }
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = base::Value(std::move(output));
  return result;
}
}  // namespace xenon::ipc

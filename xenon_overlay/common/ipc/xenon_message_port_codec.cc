// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/common/ipc/xenon_message_port_codec.h"

#include <cstdlib>
#include <limits>
#include <set>

#include "base/compiler_specific.h"
#include "base/memory/raw_ptr.h"
#include "base/numerics/safe_conversions.h"
#include "third_party/blink/public/common/messaging/web_message_port.h"
#include "v8/include/v8.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaximumMessageBytes = 64 * 1024 * 1024;
constexpr uint8_t kVersionTag = 0xff;
constexpr uint8_t kPortTag = 'M';
// Blink separated its envelope from V8 in version 16. We only emit the
// stable MessagePort host tag and ordinary V8 values, so no Blink trailer is
// needed. This supported historical envelope is also accepted by current DOM
// MessagePort deserializers. Incoming modern envelopes may carry a trailer.
constexpr uint8_t kBlinkEnvelopeVersion = 16;
constexpr uint32_t kFirstTrailerVersion = 21;

v8::Local<v8::Private> UncloneableKey(v8::Isolate* isolate) {
  return v8::Private::ForApi(
      isolate,
      v8::String::NewFromUtf8Literal(isolate, "xenon::uncloneable-object"));
}

class Writer final : public v8::ValueSerializer::Delegate {
 public:
  Writer(v8::Local<v8::Context> context,
         base::span<const v8::Local<v8::Object>> ports,
         base::RepeatingCallback<bool(v8::Local<v8::Object>)> is_port)
      : context_(context),
        ports_(ports),
        is_port_(std::move(is_port)),
        serializer(v8::Isolate::GetCurrent(), this) {}

  void ThrowDataCloneError(v8::Local<v8::String> text) override {
    auto* isolate = v8::Isolate::GetCurrent();
    auto error = v8::Exception::Error(text).As<v8::Object>();
    error
        ->CreateDataProperty(
            context_, v8::String::NewFromUtf8Literal(isolate, "name"),
            v8::String::NewFromUtf8Literal(isolate, "DataCloneError"))
        .Check();
    error
        ->CreateDataProperty(context_,
                             v8::String::NewFromUtf8Literal(isolate, "code"),
                             v8::Integer::New(isolate, 25))
        .Check();
    isolate->ThrowException(error);
  }
  bool HasCustomHostObject(v8::Isolate*) override { return true; }
  v8::Maybe<bool> IsHostObject(v8::Isolate*,
                               v8::Local<v8::Object> object) override {
    auto marked = IsMarkedAsUncloneable(context_, object);
    if (marked.IsNothing()) {
      return v8::Nothing<bool>();
    }
    return v8::Just(marked.FromJust() || is_port_.Run(object) ||
                    object->IsApiWrapper() || object->InternalFieldCount() != 0);
  }
  v8::Maybe<bool> WriteHostObject(v8::Isolate* isolate,
                                  v8::Local<v8::Object> object) override {
    for (size_t i = 0; i < ports_.size(); ++i) {
      if (object == ports_[i]) {
        serializer.WriteRawBytes(&kPortTag, 1);
        serializer.WriteUint32(base::checked_cast<uint32_t>(i));
        return v8::Just(true);
      }
    }
    ThrowMessagePortCloneError(
        isolate, "Object requires a valid MessagePort transfer entry");
    return v8::Nothing<bool>();
  }
  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate* isolate,
      v8::Local<v8::SharedArrayBuffer>) override {
    ThrowMessagePortCloneError(
        isolate, "SharedArrayBuffer cannot cross the Electron main boundary");
    return v8::Nothing<uint32_t>();
  }
  v8::Maybe<uint32_t> GetWasmModuleTransferId(
      v8::Isolate* isolate,
      v8::Local<v8::WasmModuleObject>) override {
    ThrowMessagePortCloneError(
        isolate, "WebAssembly.Module cannot cross the Electron main boundary");
    return v8::Nothing<uint32_t>();
  }
  void* ReallocateBufferMemory(void* old,
                               size_t size,
                               size_t* actual) override {
    if (size > kMaximumMessageBytes) {
      return nullptr;
    }
    *actual = size;
    return std::realloc(old, size);
  }

 private:
  v8::Local<v8::Context> context_;
  base::span<const v8::Local<v8::Object>> ports_;
  base::RepeatingCallback<bool(v8::Local<v8::Object>)> is_port_;

 public:
  v8::ValueSerializer serializer;
};

class Reader final : public v8::ValueDeserializer::Delegate {
 public:
  explicit Reader(base::span<const v8::Local<v8::Object>> ports)
      : ports_(ports) {}
  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate*) override {
    const void* tag;
    uint32_t index;
    if (!deserializer->ReadRawBytes(1, &tag) ||
        *static_cast<const uint8_t*>(tag) != kPortTag ||
        !deserializer->ReadUint32(&index) || index >= ports_.size()) {
      return {};
    }
    return ports_[index];
  }
  raw_ptr<v8::ValueDeserializer> deserializer = nullptr;

 private:
  base::span<const v8::Local<v8::Object>> ports_;
};

std::optional<base::span<const uint8_t>> V8Payload(
    base::span<const uint8_t> bytes) {
  if (bytes.size() < 2 || bytes[0] != kVersionTag) {
    return std::nullopt;
  }
  size_t cursor = 1;
  uint32_t version = 0;
  unsigned shift = 0;
  for (;;) {
    if (cursor >= bytes.size() || shift > 28) {
      return std::nullopt;
    }
    const uint8_t byte = bytes[cursor++];
    if (shift == 28 && (byte & 0xf0)) {
      return std::nullopt;
    }
    version |= uint32_t(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      break;
    }
    shift += 7;
  }
  // Before the separate envelope, the first header was V8's own header.
  if (version < kBlinkEnvelopeVersion) {
    return bytes;
  }
  // Unknown future envelope versions may change header/host-object layout.
  // Match this Chromium revision's maximum supported Blink wire version.
  if (version > 21) {
    return std::nullopt;
  }
  if (version >= kFirstTrailerVersion) {
    constexpr size_t kTrailerHeaderBytes =
        1 + sizeof(uint64_t) + sizeof(uint32_t);
    if (bytes.size() - cursor < kTrailerHeaderBytes || bytes[cursor] != 0xfe) {
      return std::nullopt;
    }
    uint64_t offset = 0;
    uint32_t length = 0;
    for (size_t j = 1; j <= sizeof(uint64_t); ++j) {
      offset = (offset << 8) | bytes[cursor + j];
    }
    for (size_t j = 1 + sizeof(uint64_t); j < kTrailerHeaderBytes; ++j) {
      length = (length << 8) | bytes[cursor + j];
    }
    cursor += kTrailerHeaderBytes;
    if (offset || length) {
      if (offset < cursor || offset > bytes.size() ||
          length != bytes.size() - offset || length < 5) {
        return std::nullopt;
      }
      auto trailer = bytes.subspan(static_cast<size_t>(offset), length);
      // The only supported Blink interface is MessagePort. Validate the
      // trailer rather than ignoring unknown exposed-interface requirements.
      if (trailer[0] != 0xa0) {
        return std::nullopt;
      }
      uint32_t count = 0;
      for (size_t j = 1; j <= sizeof(uint32_t); ++j) {
        count = (count << 8) | trailer[j];
      }
      if (count != trailer.size() - 5) {
        return std::nullopt;
      }
      for (uint8_t tag : trailer.subspan(5u)) {
        if (tag != kPortTag) {
          return std::nullopt;
        }
      }
      return bytes.subspan(cursor, static_cast<size_t>(offset) - cursor);
    }
  }
  return bytes.subspan(cursor);
}
}  // namespace

v8::Maybe<bool> MarkAsUncloneable(v8::Local<v8::Context> context,
                                v8::Local<v8::Value> value) {
  if (!value->IsObject()) {
    return v8::Just(true);
  }
  auto* isolate = v8::Isolate::GetCurrent();
  if (value->IsProxy()) {
    // Node cannot attach a private transfer-mode field to a Proxy. In
    // particular, marking must not execute its public get/set traps.
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate,
                                     "Cannot mark a Proxy as uncloneable")));
    return v8::Nothing<bool>();
  }
  return value.As<v8::Object>()->SetPrivate(
      context, UncloneableKey(isolate), v8::True(isolate));
}

v8::Maybe<bool> IsMarkedAsUncloneable(v8::Local<v8::Context> context,
                                    v8::Local<v8::Object> object) {
  return object->HasPrivate(context, UncloneableKey(v8::Isolate::GetCurrent()));
}

size_t MessagePortMessageSize(const blink::TransferableMessage& message) {
  size_t size = message.encoded_message.size();
  auto add = [&size](size_t bytes) {
    size = bytes > std::numeric_limits<size_t>::max() - size
               ? std::numeric_limits<size_t>::max()
               : size + bytes;
  };
  // Side handles also occupy transport/receiver storage. The count cap below
  // prevents many empty handles from bypassing byte admission limits.
  add(message.ports.size() > std::numeric_limits<size_t>::max() / 64
          ? std::numeric_limits<size_t>::max()
          : message.ports.size() * 64);
  for (const auto& buffer : message.array_buffer_contents_array) {
    if (!buffer) {
      return std::numeric_limits<size_t>::max();
    }
    add(buffer->contents.size());
  }
  if (!message.blobs.empty() || !message.file_system_access_tokens.empty() ||
      !message.stream_channels.empty() ||
      !message.image_bitmap_contents_array.empty() ||
      message.locked_to_sender_agent_cluster) {
    return std::numeric_limits<size_t>::max();
  }
  return size;
}

bool ValidateMessagePortMessage(const blink::TransferableMessage& message) {
  if (!(message.ports.size() <= 1024 &&
        message.array_buffer_contents_array.size() <= 1024 &&
        MessagePortMessageSize(message) <= kMaximumMessageBytes)) {
    return false;
  }
  std::set<base::UnguessableToken> ids;
  for (const auto& port : message.ports) {
    const auto& handle = port.GetHandle();
    if (!handle.IsValid() || handle.IsEntangled() ||
        !ids.insert(handle.id()).second) {
      return false;
    }
  }
  return true;
}

void ThrowMessagePortCloneError(v8::Isolate* isolate, const char* message) {
  auto error = v8::Exception::Error(
                   v8::String::NewFromUtf8(isolate, message).ToLocalChecked())
                   .As<v8::Object>();
  error
      ->CreateDataProperty(
          isolate->GetCurrentContext(),
          v8::String::NewFromUtf8Literal(isolate, "name"),
          v8::String::NewFromUtf8Literal(isolate, "DataCloneError"))
      .Check();
  error
      ->CreateDataProperty(isolate->GetCurrentContext(),
                           v8::String::NewFromUtf8Literal(isolate, "code"),
                           v8::Integer::New(isolate, 25))
      .Check();
  isolate->ThrowException(error);
}

bool EncodeMessagePortValue(
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> value,
    base::span<const v8::Local<v8::Object>> transferred_ports,
    base::RepeatingCallback<bool(v8::Local<v8::Object>)> is_port,
    blink::TransferableMessage* output) {
  Writer writer(context, transferred_ports, std::move(is_port));
  const uint8_t envelope[] = {kVersionTag, kBlinkEnvelopeVersion};
  writer.serializer.WriteRawBytes(envelope, sizeof(envelope));
  writer.serializer.WriteHeader();
  if (!writer.serializer.WriteValue(context, value).FromMaybe(false)) {
    return false;
  }
  auto [data, size] = writer.serializer.Release();
  if (transferred_ports.size() > 1024 ||
      size > kMaximumMessageBytes - transferred_ports.size() * 64) {
    std::free(data);
    ThrowMessagePortCloneError(v8::Isolate::GetCurrent(),
                               "Message exceeds the 64 MiB transport limit");
    return false;
  }
  output->owned_encoded_message.assign(data, UNSAFE_BUFFERS(data + size));
  std::free(data);
  output->encoded_message = output->owned_encoded_message;
  output->sender_agent_cluster_id =
      blink::WebMessagePort::GetEmbedderAgentClusterID();
  return true;
}

v8::MaybeLocal<v8::Value> DecodeMessagePortValue(
    v8::Local<v8::Context> context,
    const blink::TransferableMessage& message,
    base::span<const v8::Local<v8::Object>> transferred_ports) {
  // DOM-specific handles require Blink ownership/origin policies. Never
  // reinterpret them as plain objects or silently omit their side resources.
  if (!ValidateMessagePortMessage(message)) {
    return {};
  }
  auto bytes = V8Payload(message.encoded_message);
  if (!bytes || bytes->empty()) {
    return {};
  }
  auto* isolate = v8::Isolate::GetCurrent();
  Reader delegate(transferred_ports);
  v8::ValueDeserializer deserializer(isolate, bytes->data(), bytes->size(),
                                     &delegate);
  delegate.deserializer = &deserializer;
  size_t size = message.encoded_message.size();
  for (size_t i = 0; i < message.array_buffer_contents_array.size(); ++i) {
    const auto& buffer = message.array_buffer_contents_array[i];
    if (!buffer || buffer->contents.size() > kMaximumMessageBytes - size) {
      return {};
    }
    size += buffer->contents.size();
    if (buffer->javascript_resize_limit &&
        (*buffer->javascript_resize_limit < buffer->contents.size() ||
         *buffer->javascript_resize_limit > kMaximumMessageBytes)) {
      return {};
    }
    auto backing =
        buffer->javascript_resize_limit
            ? v8::ArrayBuffer::NewResizableBackingStore(
                  buffer->contents.size(),
                  base::checked_cast<size_t>(*buffer->javascript_resize_limit))
            : v8::ArrayBuffer::NewBackingStore(isolate,
                                               buffer->contents.size());
    if (!backing) {
      return {};
    }
    UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(backing->Data()),
                              backing->ByteLength()))
        .copy_from(base::span(buffer->contents));
    deserializer.TransferArrayBuffer(
        base::checked_cast<uint32_t>(i),
        v8::ArrayBuffer::New(isolate, std::move(backing)));
  }
  if (!deserializer.ReadHeader(context).FromMaybe(false)) {
    return {};
  }
  return deserializer.ReadValue(context);
}
}  // namespace xenon::ipc

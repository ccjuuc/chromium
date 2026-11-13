// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/common/ipc/xenon_message_port_codec.h"

#include <cstdlib>
#include <string>

#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "gin/converter.h"
#include "gin/test/v8_test.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "v8/include/v8.h"

namespace xenon::ipc {
namespace {
class MessagePortCodecTest : public gin::V8Test {
 protected:
  v8::Isolate* isolate() { return instance_->isolate(); }
  v8::Local<v8::Context> context() { return context_.Get(isolate()); }
  v8::Local<v8::Value> Run(const char* source) {
    return v8::Script::Compile(
               context(), gin::StringToV8(isolate(), source).As<v8::String>())
        .ToLocalChecked()
        ->Run(context())
        .ToLocalChecked();
  }
  bool Encode(v8::Local<v8::Value> value, blink::TransferableMessage* message) {
    return EncodeMessagePortValue(
        context(), value, {},
        base::BindRepeating([](v8::Local<v8::Object>) { return false; }),
        message);
  }
};

TEST_F(MessagePortCodecTest, ModernBlinkEnvelopeAndTransferArrayBuffer) {
  v8::HandleScope handles(isolate());
  v8::ValueSerializer serializer(isolate());
  serializer.WriteHeader();
  auto buffer = Run("new Uint8Array([3, 5, 8]).buffer").As<v8::ArrayBuffer>();
  serializer.TransferArrayBuffer(0, buffer);
  ASSERT_TRUE(serializer.WriteValue(context(), buffer).FromMaybe(false));
  auto [data, size] = serializer.Release();
  blink::TransferableMessage message;
  // Blink 21's empty-trailer envelope, followed by V8's structured-clone data.
  message.owned_encoded_message = {0xff, 21, 0xfe, 0, 0, 0, 0, 0,
                                   0,    0,  0,    0, 0, 0, 0};
  auto& bytes = message.owned_encoded_message;
  bytes.insert(bytes.end(), data, UNSAFE_BUFFERS(data + size));
  std::free(data);
  message.encoded_message = bytes;
  mojo_base::BigBuffer contents(3);
  auto contents_span = base::span(contents);
  contents_span[0] = 3;
  contents_span[1] = 5;
  contents_span[2] = 8;
  message.array_buffer_contents_array.push_back(
      blink::mojom::SerializedArrayBufferContents::New(std::move(contents),
                                                       std::nullopt));
  auto clone = DecodeMessagePortValue(context(), message, {}).ToLocalChecked();
  ASSERT_TRUE(clone->IsArrayBuffer());
  EXPECT_EQ(clone.As<v8::ArrayBuffer>()->ByteLength(), 3u);
  EXPECT_EQ(UNSAFE_BUFFERS(
                static_cast<uint8_t*>(clone.As<v8::ArrayBuffer>()->Data())[2]),
            8);
}

TEST_F(MessagePortCodecTest, RejectsMalformedTrailerAndUnexposedHostInterface) {
  v8::HandleScope handles(isolate());
  blink::TransferableMessage original;
  ASSERT_TRUE(Encode(Run("42"), &original));
  auto v8_bytes = base::span(original.owned_encoded_message).subspan(2u);
  blink::TransferableMessage message;
  auto& bytes = message.owned_encoded_message;
  bytes = {0xff, 21, 0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6};
  bytes.insert(bytes.end(), v8_bytes.begin(), v8_bytes.end());
  const size_t trailer_offset = bytes.size();
  ASSERT_LT(trailer_offset, 256u);
  bytes[10] = trailer_offset;
  bytes.insert(bytes.end(), {0xa0, 0, 0, 0, 1, 'M'});
  message.encoded_message = bytes;
  EXPECT_EQ(42, DecodeMessagePortValue(context(), message, {})
                    .ToLocalChecked()
                    ->Int32Value(context())
                    .FromJust());
  bytes.back() = 'F';
  EXPECT_TRUE(DecodeMessagePortValue(context(), message, {}).IsEmpty());
  bytes.back() = 'M';
  bytes[10] = 1;  // Trailer must not overlap the header.
  EXPECT_TRUE(DecodeMessagePortValue(context(), message, {}).IsEmpty());
  bytes[10] = trailer_offset;
  bytes[14] = 7;  // Advertised trailer length must fit the message.
  EXPECT_TRUE(DecodeMessagePortValue(context(), message, {}).IsEmpty());
}

TEST_F(MessagePortCodecTest,
       SharedMemoryAndFunctionsFailInsteadOfLosingMeaning) {
  v8::HandleScope handles(isolate());
  for (const char* source : {"new SharedArrayBuffer(8)", "(() => {})"}) {
    v8::TryCatch caught(isolate());
    blink::TransferableMessage message;
    EXPECT_FALSE(Encode(Run(source), &message));
    EXPECT_TRUE(caught.HasCaught());
    EXPECT_TRUE(message.ports.empty());
  }
}

TEST_F(MessagePortCodecTest, UncloneablePolicyChecksNestedObjectsDuringEncoding) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  auto marked = Run("globalThis.marked = Object.freeze({value: 1})");
  ASSERT_TRUE(MarkAsUncloneable(context(), marked).FromMaybe(false));
  for (const char* source : {"marked", "{nested: marked}", "[marked]",
                              "new Map([[1, marked]])"}) {
    SCOPED_TRACE(source);
    // Parenthesize object literals so they are expressions rather than blocks.
    std::string expression = "(" + std::string(source) + ")";
    auto value = Run(expression.c_str());
    v8::TryCatch caught(isolate());
    blink::TransferableMessage message;
    EXPECT_FALSE(Encode(value, &message));
    ASSERT_TRUE(caught.HasCaught());
    EXPECT_EQ(25, caught.Exception().As<v8::Object>()
                      ->Get(context(), gin::StringToV8(isolate(), "code"))
                      .ToLocalChecked()->Int32Value(context()).FromJust());
  }
  auto bytes = Run("new Uint8Array([1, 2])");
  ASSERT_TRUE(MarkAsUncloneable(context(), bytes).FromMaybe(false));
  blink::TransferableMessage message;
  EXPECT_TRUE(Encode(bytes, &message));
}
}  // namespace
}  // namespace xenon::ipc

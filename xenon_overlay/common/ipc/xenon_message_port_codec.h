// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_COMMON_IPC_XENON_MESSAGE_PORT_CODEC_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_MESSAGE_PORT_CODEC_H_

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "third_party/blink/public/common/messaging/transferable_message.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-maybe.h"

namespace xenon::ipc {

// Node's markAsUncloneable policy uses an own V8-private marker. Consult it
// from the serializer's host-object hook, not by traversing application data.
// V8's built-in Array/Map/Date/buffer encoders intentionally bypass that hook.
v8::Maybe<bool> MarkAsUncloneable(v8::Local<v8::Context> context,
                                v8::Local<v8::Value> value);
v8::Maybe<bool> IsMarkedAsUncloneable(v8::Local<v8::Context> context,
                                    v8::Local<v8::Object> object);

// Saturating accounting for queue admission, including all out-of-band data.
size_t MessagePortMessageSize(const blink::TransferableMessage& message);
bool ValidateMessagePortMessage(const blink::TransferableMessage& message);

// These messages use Blink's structured-clone envelope and real Mojo ports,
// so a port handed to a renderer remains an ordinary DOM MessagePort.
// Port ownership is committed by the caller only after serialization succeeds.
bool EncodeMessagePortValue(
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> value,
    base::span<const v8::Local<v8::Object>> transferred_ports,
    base::RepeatingCallback<bool(v8::Local<v8::Object>)> is_port,
    blink::TransferableMessage* output);

v8::MaybeLocal<v8::Value> DecodeMessagePortValue(
    v8::Local<v8::Context> context,
    const blink::TransferableMessage& message,
    base::span<const v8::Local<v8::Object>> transferred_ports);

void ThrowMessagePortCloneError(v8::Isolate* isolate, const char* message);

}  // namespace xenon::ipc
#endif

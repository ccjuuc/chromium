// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_worker_message.h"

#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "gin/converter.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-value-serializer.h"
#include "xenon_overlay/common/ipc/xenon_message_port_codec.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaximumMessageBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumTransfers = 1024;

v8::Local<v8::Private> CloneErrorKey(v8::Isolate* isolate) {
  return v8::Private::ForApi(
      isolate,
      v8::String::NewFromUtf8Literal(isolate, "xenon::structured-clone-error"));
}

void CloneError(v8::Isolate* isolate, v8::Local<v8::String> message) {
  auto error = v8::Exception::Error(message).As<v8::Object>();
  auto context = isolate->GetCurrentContext();
  if (error->SetPrivate(context, CloneErrorKey(isolate), v8::True(isolate))
          .IsNothing()) {
    return;
  }
  if (error
          ->CreateDataProperty(context, gin::StringToV8(isolate, "name"),
                               gin::StringToV8(isolate, "DataCloneError"))
          .IsNothing() ||
      error
          ->CreateDataProperty(context, gin::StringToV8(isolate, "code"),
                               v8::Integer::New(isolate, 25))
          .IsNothing()) {
    return;
  }
  isolate->ThrowException(error);
}
void CloneError(v8::Isolate* isolate, const char* message) {
  CloneError(isolate, gin::StringToV8(isolate, message).As<v8::String>());
}

class Writer final : public v8::ValueSerializer::Delegate {
 public:
  explicit Writer(WorkerMessage* message) : message_(message) {}
  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    CloneError(v8::Isolate::GetCurrent(), message);
  }
  bool HasCustomHostObject(v8::Isolate*) override { return true; }
  v8::Maybe<bool> IsHostObject(v8::Isolate* isolate,
                               v8::Local<v8::Object> object) override {
    auto marked = IsMarkedAsUncloneable(isolate->GetCurrentContext(), object);
    if (marked.IsNothing()) {
      return v8::Nothing<bool>();
    }
    return v8::Just(marked.FromJust() || object->IsApiWrapper() ||
                    object->InternalFieldCount() != 0);
  }
  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate* isolate,
      v8::Local<v8::SharedArrayBuffer> buffer) override {
    auto store = buffer->GetBackingStore();
    auto found = shared_ids_.find(store.get());
    if (found != shared_ids_.end()) {
      return v8::Just(found->second);
    }
    if (message_->shared.size() >= kMaximumTransfers) {
      CloneError(isolate, "Too many shared buffers in one Worker message");
      return v8::Nothing<uint32_t>();
    }
    uint32_t id = static_cast<uint32_t>(message_->shared.size());
    shared_ids_.emplace(store.get(), id);
    message_->shared.push_back(std::move(store));
    return v8::Just(id);
  }
  v8::Maybe<bool> WriteHostObject(v8::Isolate* isolate,
                                  v8::Local<v8::Object>) override {
    CloneError(isolate, "This host object cannot be cloned to a Worker");
    return v8::Nothing<bool>();
  }
  void* ReallocateBufferMemory(void* old_buffer,
                               size_t size,
                               size_t* actual_size) override {
    if (size > kMaximumMessageBytes) {
      return nullptr;
    }
    *actual_size = size;
    return std::realloc(old_buffer, size);
  }

 private:
  const raw_ptr<WorkerMessage> message_;
  std::map<v8::BackingStore*, uint32_t> shared_ids_;
};
class Reader final : public v8::ValueDeserializer::Delegate {
 public:
  explicit Reader(const WorkerMessage& message) : message_(message) {}
  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate* isolate,
      uint32_t id) override {
    if (id >= message_->shared.size()) {
      return {};
    }
    return v8::SharedArrayBuffer::New(isolate, message_->shared[id]);
  }

 private:
  const raw_ref<const WorkerMessage> message_;
};
}  // namespace

bool IsWorkerDataCloneError(v8::Local<v8::Context> context,
                            v8::Local<v8::Value> value) {
  if (!value->IsObject()) {
    return false;
  }
  v8::Local<v8::Value> marker;
  return value.As<v8::Object>()
             ->GetPrivate(context, CloneErrorKey(v8::Isolate::GetCurrent()))
             .ToLocal(&marker) &&
         marker->IsTrue();
}

PreparedWorkerMessage::PreparedWorkerMessage()
    : message_(std::make_unique<WorkerMessage>()) {}
PreparedWorkerMessage::~PreparedWorkerMessage() = default;
std::unique_ptr<PreparedWorkerMessage> PreparedWorkerMessage::Serialize(
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> value,
    v8::Local<v8::Value> transfer_list) {
  auto* isolate = v8::Isolate::GetCurrent();
  auto prepared =
      std::unique_ptr<PreparedWorkerMessage>(new PreparedWorkerMessage);
  Writer writer(prepared->message_.get());
  v8::ValueSerializer serializer(isolate, &writer);
  if (!transfer_list.IsEmpty() && !transfer_list->IsUndefined()) {
    if (!transfer_list->IsArray()) {
      CloneError(isolate, "Worker transferList must be an array");
      return nullptr;
    }
    auto array = transfer_list.As<v8::Array>();
    if (array->Length() > kMaximumTransfers) {
      CloneError(isolate, "Too many transferables in one Worker message");
      return nullptr;
    }
    for (uint32_t index = 0; index < array->Length(); ++index) {
      v8::Local<v8::Value> item;
      if (!array->Get(context, index).ToLocal(&item)) {
        return nullptr;
      }
      if (!item->IsArrayBuffer()) {
        CloneError(isolate, "Only ArrayBuffer transfer is currently supported");
        return nullptr;
      }
      auto buffer = item.As<v8::ArrayBuffer>();
      if (!buffer->IsDetachable() || buffer->WasDetached()) {
        CloneError(isolate, "ArrayBuffer is detached or not transferable");
        return nullptr;
      }
      for (const auto& source : prepared->sources_) {
        if (source.Get(isolate) == buffer) {
          CloneError(isolate,
                     "ArrayBuffer appears more than once in transferList");
          return nullptr;
        }
      }
      prepared->message_->transferred.push_back(buffer->GetBackingStore());
      prepared->sources_.emplace_back(isolate, buffer);
      serializer.TransferArrayBuffer(index, buffer);
    }
  }
  serializer.WriteHeader();
  if (!serializer.WriteValue(context, value).FromMaybe(false)) {
    return nullptr;
  }
  auto [bytes, size] = serializer.Release();
  // Release owns malloc memory. Empty data is valid only for a failed encoder,
  // which has already been rejected above.
  // The serializer owns this allocation and reports its exact byte length.
  auto serialized = UNSAFE_BUFFERS(base::span(bytes, size));
  prepared->message_->bytes.assign(serialized.begin(), serialized.end());
  writer.FreeBufferMemory(bytes);
  return prepared;
}
std::unique_ptr<WorkerMessage> PreparedWorkerMessage::Commit(
    v8::Isolate* isolate) {
  if (!message_) {
    CloneError(isolate, "The prepared message has already been committed");
    return nullptr;
  }
  // Serialization invokes getters, which may transfer one of these buffers
  // reentrantly. Revalidate the whole list before detaching any remaining
  // buffers, rather than partially committing a now-invalid transfer list.
  for (const auto& source : sources_) {
    auto buffer = source.Get(isolate);
    if (!buffer->IsDetachable() || buffer->WasDetached()) {
      CloneError(
          isolate,
          "ArrayBuffer became detached or non-transferable during cloning");
      return nullptr;
    }
  }
  for (auto& source : sources_) {
    if (!source.Get(isolate)->Detach(v8::Local<v8::Value>()).FromMaybe(false)) {
      CloneError(isolate, "ArrayBuffer could not be transferred");
      return nullptr;
    }
  }
  sources_.clear();
  return std::move(message_);
}
size_t PreparedWorkerMessage::message_size() const {
  if (!message_) {
    return 0;
  }
  size_t total = message_->bytes.size();
  std::set<const v8::BackingStore*> counted;
  for (const auto* stores : {&message_->transferred, &message_->shared}) {
    for (const auto& store : *stores) {
      if (!counted.insert(store.get()).second) {
        continue;
      }
      if (store->ByteLength() > std::numeric_limits<size_t>::max() - total) {
        return std::numeric_limits<size_t>::max();
      }
      total += store->ByteLength();
    }
  }
  return total;
}
v8::MaybeLocal<v8::Value> WorkerMessage::Deserialize(
    v8::Local<v8::Context> context) {
  auto* isolate = v8::Isolate::GetCurrent();
  Reader reader(*this);
  v8::ValueDeserializer deserializer(isolate, bytes.data(), bytes.size(),
                                     &reader);
  for (uint32_t id = 0; id < transferred.size(); ++id) {
    deserializer.TransferArrayBuffer(
        id, v8::ArrayBuffer::New(isolate, transferred[id]));
  }
  if (!deserializer.ReadHeader(context).FromMaybe(false)) {
    return {};
  }
  return deserializer.ReadValue(context);
}
}  // namespace xenon::ipc

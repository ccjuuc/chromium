// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/ipc/xenon_ipc_renderer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/v8_value_converter.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "gin/try_catch.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "ui/base/resource/resource_bundle.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"
#include "xenon_overlay/common/ipc/xenon_ipc_value_codec.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon::ipc {

namespace {

constexpr char kGlobalBindingName[] = "xenonIpcRenderer";

std::string GetRendererBootstrapSource() {
  // Pak only. A source-tree ReadFileToString / PathService lookup can
  // stall the sandboxed renderer on the broker.
  if (ui::ResourceBundle::HasSharedInstance()) {
    return ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
        IDR_XENON_IPC_RENDERER_BOOTSTRAP_JS);
  }
  return {};
}

void DefineGlobal(v8::Local<v8::Context> context,
                  const char* name,
                  v8::Local<v8::Value> value) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  // Do not use DefineProperty on chrome:// Window: non-configurable
  // descriptors can stall the renderer main thread (DevTools / WebUI
  // interceptors). Ordinary Set is enough for the bootstrap to find
  // xenonIpcRenderer / __xenonPaths.
  v8::Maybe<bool> defined =
      context->Global()->Set(context, gin::StringToV8(isolate, name), value);
  if (defined.IsNothing() || !defined.FromJust()) {
    LOG(ERROR) << "Failed to define window." << name;
  }
}

void DefineXenonPaths(v8::Local<v8::Context> context) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  // Do not call PathService in the sandboxed renderer here. DIR_HOME /
  // DIR_TEMP / DIR_SRC_TEST_DATA_ROOT can sync-IPC to the broker and
  // stall DidClearWindowObject. The JS bootstrap has path fallbacks.
  v8::Local<v8::Object> paths = v8::Object::New(isolate);
  DefineGlobal(context, "__xenonPaths", paths);
}

xenon::ipc::mojom::IpcResultPtr DisconnectedResult() {
  auto result = xenon::ipc::mojom::IpcResult::New();
  result->success = false;
  result->value = base::Value();
  result->error = "Browser ipcMain connection was closed";
  return result;
}

void ThrowIpcError(gin::Arguments* args, const std::string& error) {
  args->isolate()->ThrowException(v8::Exception::Error(
      gin::StringToV8(args->isolate(), error).As<v8::String>()));
}

bool ReadNodeInstanceSelector(gin::Arguments* args,
                              v8::Local<v8::Value> selector,
                              int32_t* instance_id,
                              std::string* owner_token) {
  owner_token->clear();
  if (!selector->IsObject()) {
    if (gin::ConvertFromV8(args->isolate(), selector, instance_id)) {
      return true;
    }
    args->ThrowTypeError("Native instance id must be an integer or selector");
    return false;
  }

  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Object> object = selector.As<v8::Object>();
  v8::Local<v8::Value> id;
  if (!object->Get(context, gin::StringToV8(args->isolate(), "instanceId"))
           .ToLocal(&id)) {
    // A selector getter/proxy may throw. Preserve that exception instead of
    // replacing it with an argument validation error in this or the caller.
    return false;
  }
  if (!gin::ConvertFromV8(args->isolate(), id, instance_id)) {
    args->ThrowTypeError(
        "Native instance selector.instanceId must be an integer");
    return false;
  }
  v8::Local<v8::Value> token;
  if (!object->Get(context, gin::StringToV8(args->isolate(), "ownerToken"))
           .ToLocal(&token)) {
    return false;
  }
  if (!gin::ConvertFromV8(args->isolate(), token, owner_token)) {
    args->ThrowTypeError(
        "Native instance selector.ownerToken must be a string");
    return false;
  }
  return true;
}

}  // namespace

// static
void XenonIpcRenderer::Install(content::RenderFrame* render_frame,
                               v8::Local<v8::Context> context) {
  VLOG(1) << "XenonIpcRenderer Install start";
  if (!render_frame || context.IsEmpty()) {
    return;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);

  v8::Local<v8::Value> existing;
  if (context->Global()
          ->Get(context, v8::String::NewFromUtf8(isolate, kGlobalBindingName)
                             .ToLocalChecked())
          .ToLocal(&existing) &&
      !existing->IsUndefined()) {
    return;
  }

  VLOG(1) << "XenonIpcRenderer Install allocating binding";
  auto binding = base::MakeRefCounted<XenonIpcRenderer>(render_frame, context);
  VLOG(1) << "XenonIpcRenderer Install created binding";
  // Browser owns the policy. Do not expose require/process/ipcRenderer until
  // the document-scoped host has approved this exact document and returned a
  // container runtime configuration.
  if (!binding->EnsureRuntimeConfig()) {
    return;
  }
  v8::Local<v8::Object> wrapper = v8::Object::New(isolate);
  VLOG(1) << "XenonIpcRenderer Install attaching methods";
  auto set_method = [&](const char* name, auto method) {
    v8::Local<v8::Function> fn;
    if (!gin::CreateFunctionTemplate(isolate,
                                     base::BindRepeating(method, binding))
             ->GetFunction(context)
             .ToLocal(&fn)) {
      LOG(ERROR) << "XenonIpcRenderer failed to create method " << name;
      return;
    }
    if (wrapper->Set(context, gin::StringToSymbol(isolate, name), fn)
            .IsNothing()) {
      LOG(ERROR) << "XenonIpcRenderer failed to set method " << name;
    }
  };
  set_method("getRuntimeConfig", &XenonIpcRenderer::GetRuntimeConfig);
  set_method("attachGuest", &XenonIpcRenderer::AttachGuest);
  set_method("send", &XenonIpcRenderer::Send);
  VLOG(1) << "XenonIpcRenderer Install set send";
  set_method("invoke", &XenonIpcRenderer::Invoke);
  set_method("sendSync", &XenonIpcRenderer::SendSync);
  set_method("requireNodeModuleSync", &XenonIpcRenderer::RequireNodeModuleSync);
  set_method("inspectNodeExportSync", &XenonIpcRenderer::InspectNodeExportSync);
  set_method("invokeNodeExportSync", &XenonIpcRenderer::InvokeNodeExportSync);
  set_method("constructNodeExportSync",
             &XenonIpcRenderer::ConstructNodeExportSync);
  set_method("constructNodeExportWithPrototypeSync",
             &XenonIpcRenderer::ConstructNodeExportWithPrototypeSync);
  set_method("invokeNodeInstanceSync",
             &XenonIpcRenderer::InvokeNodeInstanceSync);
  set_method("inspectNodeInstanceMemberSync",
             &XenonIpcRenderer::InspectNodeInstanceMemberSync);
  set_method("releaseNodeInstance", &XenonIpcRenderer::ReleaseNodeInstance);
  set_method("postMessage", &XenonIpcRenderer::PostMessage);
  set_method("setDispatchHandler", &XenonIpcRenderer::SetDispatchHandler);
  VLOG(1) << "XenonIpcRenderer Install methods attached";
  DefineGlobal(context, kGlobalBindingName, wrapper);
  VLOG(1) << "XenonIpcRenderer Install defined binding";
  DefineXenonPaths(context);
  VLOG(1) << "XenonIpcRenderer Install defined paths";
  VLOG(1) << "XenonIpcRenderer Install compiling bootstrap";
  gin::TryCatch try_catch(isolate);
  const std::string bootstrap_source = GetRendererBootstrapSource();
  VLOG(1) << "XenonIpcRenderer Install bootstrap bytes="
          << bootstrap_source.size();
  if (bootstrap_source.empty()) {
    LOG(ERROR) << "Failed to load Electron ipcRenderer bootstrap resource";
    return;
  }
  v8::Local<v8::String> source =
      v8::String::NewFromUtf8(isolate, bootstrap_source.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(bootstrap_source.length()))
          .ToLocalChecked();
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, source).ToLocal(&script) ||
      script->Run(context).IsEmpty()) {
    LOG(ERROR) << "Failed to install Electron ipcRenderer bootstrap: "
               << try_catch.GetStackTrace();
  } else {
    VLOG(1) << "XenonIpcRenderer Install done";
  }
}

XenonIpcRenderer::XenonIpcRenderer(content::RenderFrame* render_frame,
                                   v8::Local<v8::Context> context)
    : content::RenderFrameObserver(render_frame),
      isolate_(v8::Isolate::GetCurrent()),
      context_(isolate_, context) {}

XenonIpcRenderer::~XenonIpcRenderer() {
  Shutdown();
}

void XenonIpcRenderer::Shutdown() {
  // Resetting a remote destroys pending reply callbacks. Revoke their weak
  // pointers first so default replies cannot run in the released context.
  weak_factory_.InvalidateWeakPtrs();
  Dispose();
  context_.Reset();
  dispatch_handler_.Reset();
  queued_events_.clear();
  pending_invokes_.clear();
  receiver_.reset();
  node_addon_host_.reset();
  host_.reset();
  runtime_config_.reset();
}

void XenonIpcRenderer::WillReleaseScriptContext(v8::Local<v8::Context> context,
                                                int32_t world_id) {
  if (world_id == content::ISOLATED_WORLD_ID_GLOBAL && !context_.IsEmpty() &&
      context == context_.Get(isolate_)) {
    // Clearing V8 roots may release method wrappers which own the binding.
    scoped_refptr<XenonIpcRenderer> keep_alive(this);
    Shutdown();
  }
}

void XenonIpcRenderer::OnDestruct() {
  scoped_refptr<XenonIpcRenderer> keep_alive(this);
  Shutdown();
}

bool XenonIpcRenderer::EnsureHostConnected() {
  if (!render_frame()) {
    return false;
  }
  if (!host_.is_bound()) {
    render_frame()->GetBrowserInterfaceBroker().GetInterface(
        host_.BindNewPipeAndPassReceiver());
    host_.set_disconnect_handler(base::BindOnce(
        &XenonIpcRenderer::OnHostDisconnected, weak_factory_.GetWeakPtr()));
  }
  return host_.is_bound();
}

bool XenonIpcRenderer::EnsureRuntimeConfig() {
  if (runtime_config_) {
    return true;
  }
  if (!EnsureHostConnected()) {
    return false;
  }
  xenon::ipc::mojom::IpcRendererConfigPtr config;
  if (!host_->GetRuntimeConfig(&config) || !config || !render_frame()) {
    return false;
  }
  runtime_config_ = std::move(config);
  return true;
}

bool XenonIpcRenderer::EnsureConnected() {
  if (!EnsureHostConnected()) {
    return false;
  }
  if (!receiver_.is_bound()) {
    mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer;
    receiver_.Bind(renderer.InitWithNewPipeAndPassReceiver());
    host_->BindRenderer(std::move(renderer));
  }
  return true;
}

void XenonIpcRenderer::GetRuntimeConfig(gin::Arguments* args) {
  if (!EnsureRuntimeConfig()) {
    args->ThrowTypeError("Browser runtime config is unavailable");
    return;
  }

  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Object> value = v8::Object::New(isolate);
  auto set_string = [&](const char* key, const std::string& text) {
    value
        ->Set(context, gin::StringToV8(isolate, key),
              gin::StringToV8(isolate, text))
        .Check();
  };
  set_string("appName", runtime_config_->app_name);
  set_string("documentPath", runtime_config_->document_path);
  v8::Local<v8::Array> mappings = v8::Array::New(
      isolate, static_cast<int>(runtime_config_->renderer_url_mappings.size()));
  for (size_t i = 0; i < runtime_config_->renderer_url_mappings.size(); ++i) {
    const auto& mapping = runtime_config_->renderer_url_mappings[i];
    v8::Local<v8::Object> entry = v8::Object::New(isolate);
    entry
        ->Set(context, gin::StringToV8(isolate, "sourcePathPrefix"),
              gin::StringToV8(isolate, mapping->source_path_prefix))
        .Check();
    entry
        ->Set(context, gin::StringToV8(isolate, "targetBaseUrl"),
              gin::StringToV8(isolate, mapping->target_base_url))
        .Check();
    mappings->Set(context, static_cast<uint32_t>(i), entry).Check();
  }
  value->Set(context, gin::StringToV8(isolate, "rendererUrlMappings"), mappings)
      .Check();
  value
      ->Set(context, gin::StringToV8(isolate, "isGuest"),
            v8::Boolean::New(isolate, runtime_config_->is_guest))
      .Check();
  value
      ->Set(context, gin::StringToV8(isolate, "isMainFrame"),
            v8::Boolean::New(isolate, runtime_config_->is_main_frame))
      .Check();
  set_string("appVersion", runtime_config_->app_version);
  set_string("appPath", runtime_config_->app_path);
  set_string("execPath", runtime_config_->executable_path);
  set_string("userData", runtime_config_->user_data_path);
  set_string("appData", runtime_config_->app_data_path);
  set_string("localAppData", runtime_config_->local_app_data_path);
  set_string("home", runtime_config_->home_path);
  set_string("temp", runtime_config_->temp_path);
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(value);
}

bool XenonIpcRenderer::EnsureNodeAddonConnected() {
  if (node_addon_host_.is_bound()) {
    return true;
  }
  if (!EnsureConnected()) {
    return false;
  }
  host_->BindNodeAddonHost(node_addon_host_.BindNewPipeAndPassReceiver());
  node_addon_host_.set_disconnect_handler(base::BindOnce(
      [](base::WeakPtr<XenonIpcRenderer> self) {
        if (self) {
          self->node_addon_host_.reset();
        }
      },
      weak_factory_.GetWeakPtr()));
  return true;
}

bool XenonIpcRenderer::ReadChannelAndArguments(gin::Arguments* args,
                                               std::string* channel,
                                               base::Value* arguments,
                                               bool* serialized) {
  v8::LocalVector<v8::Value> all = args->GetAll();
  if (all.empty() || !gin::ConvertFromV8(args->isolate(), all[0], channel)) {
    args->ThrowTypeError("IPC channel must be a string");
    return false;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Array> array =
      v8::Array::New(args->isolate(), static_cast<int>(all.size() - 1));
  for (size_t i = 1; i < all.size(); ++i) {
    array->Set(context, static_cast<uint32_t>(i - 1), all[i]).Check();
  }
  *serialized = !IsInternalIpcChannel(*channel);
  if (*serialized) {
    std::string error;
    if (!SerializeIpcValue(args->isolate(), context, array,
                           /*rethrow_exception=*/true, arguments, &error)) {
      return false;
    }
    return true;
  }

  std::unique_ptr<base::Value> converted =
      content::V8ValueConverter::Create()->FromV8Value(array, context);
  if (!converted || !converted->is_list()) {
    args->ThrowTypeError("Invalid private IPC arguments");
    return false;
  }
  *arguments = std::move(*converted);
  return true;
}

void XenonIpcRenderer::Send(gin::Arguments* args) {
  std::string channel;
  base::Value arguments;
  bool serialized = false;
  if (!ReadChannelAndArguments(args, &channel, &arguments, &serialized)) {
    return;
  }
  if (!EnsureConnected()) {
    ThrowIpcError(args, "Browser ipcMain connection is unavailable");
    return;
  }
  host_->Send(channel, std::move(arguments));
}

void XenonIpcRenderer::PostMessage(gin::Arguments* args) {
  // The JavaScript facade rejects non-empty transfer lists until MessagePort
  // ownership can be represented by the transport. The message itself uses
  // the same structured-clone wire format as send().
  Send(args);
}

void XenonIpcRenderer::AttachGuest(gin::Arguments* args) {
  auto* isolate = args->isolate();
  auto context = args->GetHolderCreationContext();
  v8::Local<v8::Value> element_value, preferences_value;
  if (!args->GetNext(&element_value) || !args->GetNext(&preferences_value)) {
    args->ThrowTypeError("attachGuest requires an iframe and preferences");
    return;
  }
  auto element = blink::WebElement::FromV8Value(isolate, element_value);
  auto* frame = element.IsNull()
                    ? nullptr
                    : blink::WebFrame::FromFrameOwnerElement(element);
  auto preferences = content::V8ValueConverter::Create()->FromV8Value(
      preferences_value, context);
  if (!frame || !frame->IsWebLocalFrame() || !preferences ||
      !preferences->is_dict()) {
    args->ThrowTypeError("Guest frame must be connected and local");
    return;
  }
  if (!EnsureConnected()) {
    ThrowIpcError(args, "Browser ipcMain connection is unavailable");
    return;
  }
  auto resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
  const uint64_t request_id = AddPendingInvoke(resolver, false);
  auto callback = base::BindOnce(&XenonIpcRenderer::OnInvoke,
                                 weak_factory_.GetWeakPtr(), request_id);
  host_->AttachGuest(frame->ToWebLocalFrame()->GetLocalFrameToken().value(),
                     std::move(*preferences),
                     mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                         std::move(callback), DisconnectedResult()));
  args->Return(resolver->GetPromise());
}

void XenonIpcRenderer::Invoke(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  std::string channel;
  base::Value arguments;
  bool serialized = false;
  if (!ReadChannelAndArguments(args, &channel, &arguments, &serialized)) {
    return;
  }
  if (!EnsureConnected()) {
    resolver
        ->Reject(context,
                 v8::Exception::Error(
                     gin::StringToV8(
                         isolate, "Browser ipcMain connection is unavailable")
                         .As<v8::String>()))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  const uint64_t request_id = AddPendingInvoke(resolver, serialized);
  auto callback = base::BindOnce(&XenonIpcRenderer::OnInvoke,
                                 weak_factory_.GetWeakPtr(), request_id);
  host_->Invoke(channel, std::move(arguments),
                mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                    std::move(callback), DisconnectedResult()));
  args->Return(resolver->GetPromise());
}

uint64_t XenonIpcRenderer::AddPendingInvoke(
    v8::Local<v8::Promise::Resolver> resolver,
    bool serialized) {
  const uint64_t request_id = next_invoke_id_++;
  pending_invokes_.emplace(
      request_id,
      PendingInvoke{v8::Global<v8::Promise::Resolver>(isolate_, resolver),
                    serialized});
  return request_id;
}

void XenonIpcRenderer::OnInvoke(uint64_t request_id,
                                xenon::ipc::mojom::IpcResultPtr result) {
  scoped_refptr<XenonIpcRenderer> keep_alive(this);
  auto pending = pending_invokes_.find(request_id);
  if (context_.IsEmpty() || pending == pending_invokes_.end()) {
    return;
  }
  PendingInvoke invoke = std::move(pending->second);
  pending_invokes_.erase(pending);
  v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  // Renderer isolates use scoped microtasks. Mojo replies arrive outside the
  // original JavaScript call, so resolving or rejecting here requires an
  // active scope; Blink performs the actual checkpoint after this task.
  v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Local<v8::Promise::Resolver> resolver = invoke.resolver.Get(isolate_);
  if (!result || !result->success) {
    const std::string error = result ? result->error : "Invalid IPC reply";
    resolver
        ->Reject(context,
                 v8::Exception::Error(
                     gin::StringToV8(isolate_, error).As<v8::String>()))
        .Check();
    return;
  }
  v8::Local<v8::Value> value;
  if (invoke.serialized) {
    std::string error;
    if (!DeserializeIpcValue(isolate_, context, result->value, &error)
             .ToLocal(&value)) {
      resolver
          ->Reject(context, v8::Exception::Error(
                                gin::StringToV8(
                                    isolate_,
                                    error.empty() ? "Invalid IPC reply" : error)
                                    .As<v8::String>()))
          .Check();
      return;
    }
  } else {
    value =
        content::V8ValueConverter::Create()->ToV8Value(result->value, context);
  }
  resolver->Resolve(context, value).Check();
}

void XenonIpcRenderer::SendSync(gin::Arguments* args) {
  std::string channel;
  base::Value arguments;
  bool serialized = false;
  if (!ReadChannelAndArguments(args, &channel, &arguments, &serialized)) {
    return;
  }
  if (!EnsureConnected()) {
    ThrowIpcError(args, "Browser ipcMain connection is unavailable");
    return;
  }
  xenon::ipc::mojom::IpcResultPtr result;
  if (!host_->SendSync(channel, std::move(arguments), &result) || !result) {
    ThrowIpcError(args, "Synchronous Browser IPC failed");
    return;
  }
  if (!result->success) {
    ThrowIpcError(args, result->error);
    return;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  if (serialized) {
    std::string error;
    v8::Local<v8::Value> value;
    if (!DeserializeIpcValue(isolate_, context, result->value, &error)
             .ToLocal(&value)) {
      ThrowIpcError(args,
                    error.empty() ? "Invalid synchronous IPC reply" : error);
      return;
    }
    args->GetFunctionCallbackInfo()->GetReturnValue().Set(value);
    return;
  }
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(
      content::V8ValueConverter::Create()->ToV8Value(result->value, context));
}

void XenonIpcRenderer::RequireNodeModuleSync(gin::Arguments* args) {
  std::string module_path;
  if (!args->GetNext(&module_path)) {
    args->ThrowTypeError("Native module path must be a string");
    return;
  }
  if (!EnsureNodeAddonConnected()) {
    args->ThrowTypeError("Utility native module connection is unavailable");
    return;
  }
  xenon::ipc::mojom::IpcResultPtr result;
  if (!node_addon_host_->RequireNodeModuleSync(module_path, &result) ||
      !result) {
    args->ThrowTypeError("Synchronous native module load failed");
    return;
  }
  if (!result->success) {
    args->ThrowTypeError(result->error);
    return;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(
      content::V8ValueConverter::Create()->ToV8Value(result->value, context));
}

void XenonIpcRenderer::InvokeNodeExportSync(gin::Arguments* args) {
  v8::LocalVector<v8::Value> all = args->GetAll();
  std::string module_path;
  std::string function_name;
  if (all.size() < 2 ||
      !gin::ConvertFromV8(args->isolate(), all[0], &module_path) ||
      !gin::ConvertFromV8(args->isolate(), all[1], &function_name)) {
    args->ThrowTypeError("Native module path and export name must be strings");
    return;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Array> array =
      v8::Array::New(args->isolate(), static_cast<int>(all.size() - 2));
  for (size_t i = 2; i < all.size(); ++i) {
    array->Set(context, static_cast<uint32_t>(i - 2), all[i]).Check();
  }
  std::unique_ptr<base::Value> converted =
      content::V8ValueConverter::Create()->FromV8Value(array, context);
  if (!converted || !converted->is_list()) {
    args->ThrowTypeError(
        "Native addon arguments are not structured-clone compatible");
    return;
  }
  if (!EnsureNodeAddonConnected()) {
    args->ThrowTypeError("Utility native module connection is unavailable");
    return;
  }
  xenon::ipc::mojom::IpcResultPtr result;
  uint64_t pending_promise_id = 0;
  if (!node_addon_host_->InvokeNodeExportSync(module_path, function_name,
                                              std::move(*converted), &result,
                                              &pending_promise_id) ||
      !result) {
    args->ThrowTypeError("Synchronous native addon invocation failed");
    return;
  }
  ReturnNativeInvokeResult(args, std::move(result), pending_promise_id);
}

void XenonIpcRenderer::ConstructNodeExportSync(gin::Arguments* args) {
  ConstructNodeExport(args, false);
}

void XenonIpcRenderer::ConstructNodeExportWithPrototypeSync(
    gin::Arguments* args) {
  ConstructNodeExport(args, true);
}

void XenonIpcRenderer::ConstructNodeExport(gin::Arguments* args,
                                           bool with_prototype) {
  v8::LocalVector<v8::Value> all = args->GetAll();
  const size_t first_argument = with_prototype ? 3 : 2;
  std::string module_path;
  std::string export_path;
  if (all.size() < first_argument ||
      !gin::ConvertFromV8(args->isolate(), all[0], &module_path) ||
      !gin::ConvertFromV8(args->isolate(), all[1], &export_path)) {
    args->ThrowTypeError(
        "Native module path and constructor name must be strings");
    return;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  bool conversion_threw = false;
  const auto convert = [&](v8::Local<v8::Value> value) {
    v8::TryCatch try_catch(args->isolate());
    auto converted =
        content::V8ValueConverter::Create()->FromV8Value(value, context);
    if (try_catch.HasCaught()) {
      conversion_threw = true;
      try_catch.ReThrow();
    }
    return converted;
  };
  base::Value prototype_properties(base::DictValue{});
  if (with_prototype) {
    std::unique_ptr<base::Value> properties = convert(all[2]);
    if (conversion_threw) {
      return;
    }
    if (!properties || !properties->is_dict()) {
      args->ThrowTypeError("Native prototype properties must be an object");
      return;
    }
    prototype_properties = std::move(*properties);
  }
  v8::Local<v8::Array> array = v8::Array::New(
      args->isolate(), static_cast<int>(all.size() - first_argument));
  for (size_t i = first_argument; i < all.size(); ++i) {
    array->Set(context, static_cast<uint32_t>(i - first_argument), all[i])
        .Check();
  }
  std::unique_ptr<base::Value> converted = convert(array);
  if (conversion_threw) {
    return;
  }
  if (!converted || !converted->is_list()) {
    args->ThrowTypeError(
        "Native addon arguments are not structured-clone compatible");
    return;
  }
  if (!EnsureNodeAddonConnected()) {
    args->ThrowTypeError("Utility native module connection is unavailable");
    return;
  }
  xenon::ipc::mojom::IpcResultPtr result;
  if (!node_addon_host_->ConstructNodeExportSync(
          module_path, export_path, std::move(*converted),
          std::move(prototype_properties), &result) ||
      !result) {
    args->ThrowTypeError("Synchronous native construct failed");
    return;
  }
  if (!result->success) {
    args->ThrowTypeError(result->error);
    return;
  }
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(
      content::V8ValueConverter::Create()->ToV8Value(result->value, context));
}

void XenonIpcRenderer::InvokeNodeInstanceSync(gin::Arguments* args) {
  v8::LocalVector<v8::Value> all = args->GetAll();
  std::string module_path;
  int32_t instance_id = 0;
  std::string owner_token;
  std::string method_name;
  if (all.size() < 3 ||
      !gin::ConvertFromV8(args->isolate(), all[0], &module_path) ||
      !gin::ConvertFromV8(args->isolate(), all[2], &method_name)) {
    args->ThrowTypeError(
        "Native module path, instance id, and method name are required");
    return;
  }
  if (!ReadNodeInstanceSelector(args, all[1], &instance_id, &owner_token)) {
    return;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Array> array =
      v8::Array::New(args->isolate(), static_cast<int>(all.size() - 3));
  for (size_t i = 3; i < all.size(); ++i) {
    array->Set(context, static_cast<uint32_t>(i - 3), all[i]).Check();
  }
  std::unique_ptr<base::Value> converted =
      content::V8ValueConverter::Create()->FromV8Value(array, context);
  if (!converted || !converted->is_list()) {
    args->ThrowTypeError(
        "Native addon arguments are not structured-clone compatible");
    return;
  }
  if (!EnsureNodeAddonConnected()) {
    args->ThrowTypeError("Utility native module connection is unavailable");
    return;
  }
  xenon::ipc::mojom::IpcResultPtr result;
  uint64_t pending_promise_id = 0;
  if (!node_addon_host_->InvokeNodeInstanceSync(
          module_path, instance_id, method_name, std::move(*converted),
          owner_token, &result, &pending_promise_id) ||
      !result) {
    args->ThrowTypeError("Synchronous native instance invocation failed");
    return;
  }
  ReturnNativeInvokeResult(args, std::move(result), pending_promise_id);
}

void XenonIpcRenderer::ReturnNativeInvokeResult(
    gin::Arguments* args,
    xenon::ipc::mojom::IpcResultPtr result,
    uint64_t pending_promise_id) {
  if (!result->success) {
    args->ThrowTypeError(result->error);
    return;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  if (!pending_promise_id) {
    args->GetFunctionCallbackInfo()->GetReturnValue().Set(
        content::V8ValueConverter::Create()->ToV8Value(result->value, context));
    return;
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }
  const uint64_t request_id = AddPendingInvoke(resolver, false);
  auto disconnected = xenon::ipc::mojom::IpcResult::New();
  disconnected->success = false;
  disconnected->error = "Native Promise connection was closed";
  // Resetting this remote on disconnect destroys the callback and rejects
  // this resolver. It does not cancel unrelated Browser IPC requests.
  node_addon_host_->AwaitNodePromise(
      pending_promise_id,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonIpcRenderer::OnInvoke,
                         weak_factory_.GetWeakPtr(), request_id),
          std::move(disconnected)));
  args->Return(resolver->GetPromise());
}

void XenonIpcRenderer::InspectNodeExportSync(gin::Arguments* args) {
  std::string module_path;
  std::string export_path;
  if (!args->GetNext(&module_path) || !args->GetNext(&export_path)) {
    args->ThrowTypeError("Native module path and export path are required");
    return;
  }
  if (!EnsureNodeAddonConnected()) {
    args->ThrowTypeError("Utility native module connection is unavailable");
    return;
  }

  xenon::ipc::mojom::IpcResultPtr result;
  if (!node_addon_host_->InspectNodeExportSync(module_path, export_path,
                                               &result) ||
      !result) {
    args->ThrowTypeError("Synchronous native export inspection failed");
    return;
  }
  if (!result->success) {
    args->ThrowTypeError(result->error);
    return;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(
      content::V8ValueConverter::Create()->ToV8Value(result->value, context));
}

void XenonIpcRenderer::InspectNodeInstanceMemberSync(gin::Arguments* args) {
  v8::LocalVector<v8::Value> all = args->GetAll();
  std::string module_path;
  int32_t instance_id = 0;
  std::string owner_token;
  std::string property_name;
  if (all.size() < 3 ||
      !gin::ConvertFromV8(args->isolate(), all[0], &module_path) ||
      !gin::ConvertFromV8(args->isolate(), all[2], &property_name)) {
    args->ThrowTypeError(
        "Native module path, instance id, and property name are required");
    return;
  }
  if (!ReadNodeInstanceSelector(args, all[1], &instance_id, &owner_token)) {
    return;
  }
  if (!EnsureNodeAddonConnected()) {
    args->ThrowTypeError("Utility native module connection is unavailable");
    return;
  }

  xenon::ipc::mojom::IpcResultPtr result;
  if (!node_addon_host_->InspectNodeInstanceMemberSync(
          module_path, instance_id, property_name, owner_token, &result) ||
      !result) {
    args->ThrowTypeError("Synchronous native instance inspection failed");
    return;
  }
  if (!result->success) {
    args->ThrowTypeError(result->error);
    return;
  }
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(
      content::V8ValueConverter::Create()->ToV8Value(result->value, context));
}

void XenonIpcRenderer::ReleaseNodeInstance(gin::Arguments* args) {
  // Finalizers can outlive their document or its native connection. Cleanup
  // must never create a new connection (and therefore a new instance owner).
  if (!node_addon_host_.is_bound() || !node_addon_host_.is_connected()) {
    return;
  }
  v8::LocalVector<v8::Value> all = args->GetAll();
  std::string module_path;
  int32_t instance_id = 0;
  std::string owner_token;
  if (all.size() < 3 ||
      !gin::ConvertFromV8(args->isolate(), all[0], &module_path) ||
      !gin::ConvertFromV8(args->isolate(), all[1], &instance_id) ||
      !gin::ConvertFromV8(args->isolate(), all[2], &owner_token)) {
    args->ThrowTypeError(
        "Native module path, instance id, and owner token are required");
    return;
  }
  node_addon_host_->ReleaseNodeInstance(module_path, instance_id, owner_token);
}

void XenonIpcRenderer::SetDispatchHandler(gin::Arguments* args) {
  if (context_.IsEmpty()) {
    args->ThrowTypeError("The IPC document has been released");
    return;
  }
  v8::Local<v8::Value> value = args->PeekNext();
  if (value.IsEmpty() || !value->IsFunction()) {
    args->ThrowTypeError("setDispatchHandler expects a function");
    return;
  }
  dispatch_handler_.Reset(args->isolate(), value.As<v8::Function>());
  std::vector<std::pair<std::string, base::Value>> queued;
  queued.swap(queued_events_);
  for (const auto& [channel, arguments] : queued) {
    DispatchNow(channel, arguments);
  }
}

void XenonIpcRenderer::Dispatch(const std::string& channel,
                                base::Value arguments) {
  if (context_.IsEmpty()) {
    return;
  }
  if (dispatch_handler_.IsEmpty()) {
    queued_events_.emplace_back(channel, std::move(arguments));
    return;
  }
  DispatchNow(channel, arguments);
}

void XenonIpcRenderer::DispatchNow(const std::string& channel,
                                   const base::Value& arguments) {
  scoped_refptr<XenonIpcRenderer> keep_alive(this);
  if (dispatch_handler_.IsEmpty() || context_.IsEmpty()) {
    return;
  }
  v8::Isolate* isolate = isolate_;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = context_.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Local<v8::Value> decoded_arguments;
  if (IsSerializedIpcValue(arguments)) {
    std::string error;
    if (!DeserializeIpcValue(isolate, context, arguments, &error)
             .ToLocal(&decoded_arguments)) {
      LOG(ERROR) << "Failed to decode ipcRenderer event '" << channel
                 << "': " << error;
      return;
    }
  } else {
    decoded_arguments =
        content::V8ValueConverter::Create()->ToV8Value(arguments, context);
  }
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate, channel),
                                 decoded_arguments};
  gin::TryCatch try_catch(isolate);
  if (dispatch_handler_.Get(isolate)
          ->Call(context, context->Global(), std::size(argv), argv)
          .IsEmpty()) {
    LOG(ERROR) << "ipcRenderer listener failed for " << channel << ": "
               << try_catch.GetStackTrace();
  }
}

void XenonIpcRenderer::RejectPendingInvokes(const std::string& error) {
  if (context_.IsEmpty() || pending_invokes_.empty()) {
    pending_invokes_.clear();
    return;
  }
  v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Local<v8::Value> exception =
      v8::Exception::Error(gin::StringToV8(isolate_, error).As<v8::String>());
  for (auto& entry : pending_invokes_) {
    entry.second.resolver.Get(isolate_)->Reject(context, exception).Check();
  }
  pending_invokes_.clear();
}

void XenonIpcRenderer::OnHostDisconnected() {
  scoped_refptr<XenonIpcRenderer> keep_alive(this);
  RejectPendingInvokes("Browser ipcMain connection was closed");
  Shutdown();
}

}  // namespace xenon::ipc

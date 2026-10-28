// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "node_api.h"
#include "uv.h"

namespace {

struct CloseState {
  uv_timer_t timer;
  NAPI_RAW_PTR_EXCLUSION int* closed;
  NAPI_RAW_PTR_EXCLUSION int* ticks;
};

struct FailureTimerConfig {
  NAPI_RAW_PTR_EXCLUSION uv_loop_t* loop = nullptr;
  NAPI_RAW_PTR_EXCLUSION int* closed = nullptr;
  NAPI_RAW_PTR_EXCLUSION int* ticks = nullptr;
};

FailureTimerConfig failure_timer;

void CloseTimer(uv_handle_t* handle) {
  auto* state = static_cast<CloseState*>(handle->data);
  ++*state->closed;
  delete state;
}

void CleanupTimer(void* arg) {
  auto* state = static_cast<CloseState*>(arg);
  uv_timer_stop(&state->timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&state->timer), CloseTimer);
}

int StartCloseTimer(napi_env env, uv_loop_t* loop, int* closed, int* ticks) {
  auto* state = new CloseState{{}, closed, ticks};
  int result = uv_timer_init(loop, &state->timer);
  if (result != 0) {
    delete state;
    return result;
  }
  state->timer.data = state;
  result = uv_timer_start(
      &state->timer,
      [](uv_timer_t* timer) {
        ++*static_cast<CloseState*>(timer->data)->ticks;
      },
      0, 1);
  if (result != 0) {
    CleanupTimer(state);
    return result;
  }
  napi_status status = napi_add_env_cleanup_hook(env, CleanupTimer, state);
  if (status != napi_ok) {
    CleanupTimer(state);
    return static_cast<int>(status);
  }
  return 0;
}

napi_value Initialize(napi_env env, napi_value exports) {
  if (failure_timer.loop) {
    const FailureTimerConfig config = failure_timer;
    failure_timer = {};
    StartCloseTimer(env, config.loop, config.closed, config.ticks);
    return nullptr;
  }
  napi_value answer;
  if (napi_create_int32(env, 42, &answer) != napi_ok ||
      napi_set_named_property(env, exports, "answer", answer) != napi_ok) {
    return nullptr;
  }
  return exports;
}

napi_module module = {1,
                      0,
                      __FILE__,
                      Initialize,
                      "test_static_addon",
                      nullptr,
                      {nullptr, nullptr, nullptr, nullptr}};

// 故意不导出 napi_register_module_v1，覆盖真实 addon 的构造期注册路径。
__attribute__((constructor)) void Register() {
  napi_module_register(&module);
}

}  // namespace

// 测试回调只清理 native 数据，绝不在 env 销毁后进入 N-API 或 JS。
NAPI_EXTERN int XenonTestStartCloseTimer(napi_env env,
                                         uv_loop_t* loop,
                                         int* closed,
                                         int* ticks) {
  return StartCloseTimer(env, loop, closed, ticks);
}

NAPI_EXTERN uv_close_cb XenonTestGetCloseCallback() {
  return CloseTimer;
}

NAPI_EXTERN void XenonTestPrepareInitializationFailure(uv_loop_t* loop,
                                                       int* closed,
                                                       int* ticks) {
  failure_timer = {loop, closed, ticks};
}

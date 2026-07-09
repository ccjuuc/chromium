# Mojo 回调与 V8 微任务说明

本文整理 `window.xenon.registerTool()` / `PageHandler::TestExecutePageTool()` 链路中涉及的两个容易混淆的点：

1. `mojo::WrapCallbackWithDefaultInvokeIfNotRun` 的作用和使用边界。
2. V8 `MicrotasksScope` / JavaScript microtask 的含义，以及为什么这里使用 `kDoNotRunMicrotasks`。

相关代码：

- `xenon_overlay/chrome/browser/ui/webui/xenon_page_handler.cc`
- `xenon_overlay/chrome/renderer/js_xenon_api.cc`
- `mojo/public/cpp/bindings/callback_helpers.h`
- `v8/include/v8-microtask-queue.h`

## 背景链路

页面侧注册工具：

```ts
window.xenon.registerTool(
    'test_calculator',
    '一个由 WebUI 注册的测试加法计算器',
    schema,
    async (inputJson) => {
      const {a, b} = JSON.parse(inputJson);
      await new Promise(resolve => setTimeout(resolve, 100));
      return `计算结果为: ${a + b}`;
    });
```

整体调用方向：

```text
WebUI JS
  -> window.xenon.registerTool(...)
  -> renderer C++ JSXenonApi::RegisterTool()
  -> browser C++ XenonPageHostImpl::RegisterTool()
  -> browser 保存 pending_remote<XenonToolExecutor>

WebUI JS
  -> PageHandler.testExecutePageTool(...)
  -> browser C++ XenonPageHandler::TestExecutePageTool()
  -> renderer C++ XenonToolExecutorImpl::Execute()
  -> 调用页面 JS callback
  -> 返回 string 或 Promise<string>
  -> browser PageHandler callback 返回给 WebUI JS
```

这条链路跨了两层 Mojo：

- WebUI `PageHandler`：页面调用 browser，browser 必须给页面返回 response。
- `XenonToolExecutor`：browser 调 renderer，renderer 必须给 browser 返回工具执行结果。

任何一层 response callback 被静默丢弃，都可能导致 Mojo DCHECK。

## JavaScript microtask 是什么

JavaScript 事件循环里可以粗略分为：

- 当前同步调用栈。
- microtask queue。
- 后续普通 task queue。

常见 microtask 来源：

- `Promise.then(...)`
- `catch(...)`
- `finally(...)`
- `await` 后续代码
- `queueMicrotask(...)`

示例：

```js
console.log('A');

setTimeout(() => console.log('timeout'), 0);

Promise.resolve().then(() => console.log('promise'));

console.log('B');
```

通常输出：

```text
A
B
promise
timeout
```

原因是：

1. 当前同步代码先执行。
2. 当前 task 结束后，先执行 microtask。
3. 之后才进入下一轮普通 task，例如 `setTimeout`。

microtask 的特点：

- 优先级高于普通 task。
- 一般在当前 JS 调用栈清空后尽快执行。
- microtask 执行期间如果继续塞入新的 microtask，事件循环会继续清空队列。
- 因此 microtask 滥用可能导致后续普通 task 被饿住。

## V8 MicrotasksScope 的作用

V8 头文件 `v8/include/v8-microtask-queue.h` 对 `MicrotasksScope` 的说明是：

- 当 isolate 使用 `MicrotasksPolicy::kScoped` 时，每次非 primitive 的 V8 调用都应该处在某个 `MicrotasksScope` 内。
- `kRunMicrotasks`：最外层 scope 退出时执行 microtask checkpoint。
- `kDoNotRunMicrotasks`：标记这次调用不负责触发 microtask 执行。

当前 renderer executor 中的代码：

```cpp
v8::MicrotasksScope microtasks(
    isolate_,
    context->GetMicrotaskQueue(),
    v8::MicrotasksScope::kDoNotRunMicrotasks);
```

它表达的是：

1. 现在 C++ 要进入 V8 调用页面 JS callback。
2. 这次调用属于某个明确的 microtask scope。
3. 如果 JS callback 创建 Promise 或 `queueMicrotask()`，这些 microtask 可以正常入队。
4. 当前 C++ 调用结束时，不主动清空 microtask queue。
5. microtask 交给 Blink/event loop 后续正常 checkpoint 处理。

注意：`kDoNotRunMicrotasks` 不是禁用 Promise。

它只是说“这次 C++ 进入 JS 不负责主动 flush Promise continuation”。

## 为什么这里不用 kRunMicrotasks

`XenonToolExecutorImpl::Execute()` 是 Mojo 从 browser 调进 renderer 后，renderer C++ 再调用页面 JS callback：

```cpp
v8::MaybeLocal<v8::Value> maybe_result =
    js_callback->Call(context, context->Global(), 1, argv);
```

如果这里使用 `kRunMicrotasks`，当前 scope 退出时可能立刻执行 Promise continuation。调用栈可能变成：

```text
browser Mojo
  -> renderer Execute()
    -> C++ 调 JS callback
      -> JS resolve Promise
    -> MicrotasksScope 退出
      -> 立刻跑 Promise.then / await 后续
        -> 可能再次调用 Mojo/browser
```

这会引入额外重入风险：

- 当前 Mojo request 还没完全退出，就开始跑 JS 后续逻辑。
- JS 后续逻辑可能再次调用 WebUI Mojo。
- 页面、frame、receiver、response callback 的生命周期更难判断。

因此在这种 Mojo executor 回调入口，使用 `kDoNotRunMicrotasks` 更保守：规范地进入 V8，但不在这个点额外推动 Promise 队列。

## Promise 返回值如何处理

页面 callback 可能返回同步 string：

```js
(input) => 'ok'
```

也可能返回 Promise：

```js
async (input) => {
  await someAsyncWork();
  return 'ok';
}
```

renderer 侧逻辑：

```cpp
v8::Local<v8::Value> result = maybe_result.ToLocalChecked();
if (result->IsPromise()) {
  v8::Local<v8::Promise> promise = result.As<v8::Promise>();
  promise->Then(context, resolved_fn, rejected_fn);
} else {
  std::string result_str;
  if (gin::ConvertFromV8(isolate_, result, &result_str)) {
    std::move(callback).Run(result_str);
  } else {
    std::move(callback).Run(std::nullopt);
  }
}
```

重点：

- 同步返回值直接转换并回 Mojo callback。
- Promise 返回值注册 `Then()`，等待 Promise 后续 resolve/reject。
- `Then()` 的回调本身就是 microtask 机制的一部分。
- 当前 `MicrotasksScope(kDoNotRunMicrotasks)` 不阻止 Promise 后续执行，只是不在当前 C++ 调用栈主动 flush。

## Mojo response callback 的要求

Mojo 带 response 的方法，例如：

```mojom
interface PageHandler {
  TestExecutePageTool(string name, string input_json)
      => (bool success, string result);
};
```

C++ 实现收到的 callback 不能被静默销毁。它必须满足其中之一：

1. 被 `Run()` 一次，向调用方返回 response。
2. 对应 pipe 已关闭。

否则 Debug/DCHECK 构建里会报类似错误：

```text
PageHandler::TestExecutePageToolCallback was destroyed without first either
being run or its corresponding binding being closed.
```

这类错误通常说明：

- callback 被移动到异步闭包里。
- 异步闭包因为 Mojo pipe 断开、任务取消、对象析构等原因被销毁。
- callback 没有被 `Run()`。
- 但调用方 pipe 仍然连着，所以 Mojo 判断这是接口实现错误。

## WrapCallbackWithDefaultInvokeIfNotRun

`mojo::WrapCallbackWithDefaultInvokeIfNotRun` 位于：

```text
mojo/public/cpp/bindings/callback_helpers.h
```

用法示例：

```cpp
remote->DoWork(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
    base::BindOnce(&Foo::OnDone, weak_factory_.GetWeakPtr()),
    false,
    "callback dropped"));
```

含义：

- 如果 callback 正常执行，走真实参数。
- 如果 callback 没执行就析构，用默认参数执行一次。

它的核心原理是 RAII：

```cpp
~CallbackWithDeleteHelper() {
  if (delete_callback_) {
    std::move(delete_callback_).Run();
  }
}

void Run(Args... args) {
  delete_callback_.Reset();
  std::move(callback_).Run(std::forward<Args>(args)...);
}
```

正常路径：

```text
Run(real_args...)
  -> Reset delete_callback_
  -> Run 原始 callback
  -> 析构时不再执行默认回调
```

异常/断开路径：

```text
callback 对象没 Run 就析构
  -> delete_callback_ 仍然存在
  -> 析构里用默认参数 Run 原始 callback
```

## 本次链路里的使用

browser 侧 `PageHandler::TestExecutePageTool`：

```cpp
tool.executor->Execute(
    input_json,
    base::BindOnce(
        [](TestExecutePageToolCallback cb,
           const std::optional<std::string>& result) {
          if (result.has_value()) {
            std::move(cb).Run(true, *result);
          } else {
            std::move(cb).Run(false, "Tool execution returned nullopt");
          }
        },
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            std::move(callback), false,
            "Tool execution failed (callback dropped)")));
```

作用：

- renderer executor 正常返回时，WebUI 收到真实结果。
- renderer executor 断开、任务被丢、response callback 被销毁时，WebUI 收到失败结果。
- 避免 `PageHandler::TestExecutePageToolCallback` 被静默销毁触发 DCHECK。

renderer 侧 `XenonToolExecutorImpl::Execute`：

```cpp
callback = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
    std::move(callback), std::nullopt);
```

作用：

- 如果 renderer 执行 JS callback 过程异常退出、Promise handler 创建失败、闭包被丢，browser 侧至少收到 `nullopt`。
- browser 侧再把 `nullopt` 转换成 WebUI 可读的失败信息。

## 注意事项

`WrapCallbackWithDefaultInvokeIfNotRun` 不是 timeout。

如果对端一直不返回，pipe 也不断，callback 也不析构，它不会自动触发。需要 timeout 时，应额外使用 `PostDelayedTask`、pending map 或专门的 request state 管理。

默认 callback 在析构发生的 sequence 上执行。

对 Mojo async callback 通常没问题，因为 callback 的执行和销毁都发生在 `mojo::Remote` 绑定的 sequence 上。但如果 callback 被传到复杂线程模型里，需要谨慎。

不要把 wrapped callback 传入很深的调用链。

它的类型仍然只是 `base::OnceCallback`，读代码的人看不出“析构会执行回调”。推荐在 Mojo 边界附近使用。

## 推荐实践

跨 Mojo + V8 + JS Promise 的代码，建议遵守：

1. Mojo response callback 必须保证最终 `Run()` 或有明确断开处理。
2. 跨 pipe 的 callback 优先在 Mojo 边界使用 `WrapCallbackWithDefaultInvokeIfNotRun` 做兜底。
3. C++ 主动调用 JS 时，进入 `v8::HandleScope`、`v8::Context::Scope` 和合适的 `v8::MicrotasksScope`。
4. Mojo 回调入口调用页面 JS，默认使用 `kDoNotRunMicrotasks`，避免在当前 Mojo 调用栈主动 flush Promise continuation。
5. 对 `MaybeLocal` 使用 `ToLocal()` 分支处理，避免不必要的 `ToLocalChecked()` 终止 renderer。
6. 页面 JS callback 可能返回同步值，也可能返回 Promise，两个路径都要保证 Mojo callback 有回应。
7. `TryCatch` 捕获 JS callback 抛错，转换成 `nullopt` 或明确错误，不让异常穿透到 Mojo/V8 边界外。

## 简短结论

`MicrotasksScope(kDoNotRunMicrotasks)` 解决的是 C++ 进入 V8 调用 JS 时的 microtask 边界问题。

`WrapCallbackWithDefaultInvokeIfNotRun` 解决的是 Mojo response callback 不能被静默丢弃的问题。

两者关注点不同，但在 `PageTool` 链路里会一起出现：C++ 需要安全地调用 JS，也需要确保跨 Mojo 的异步结果最终有回应。

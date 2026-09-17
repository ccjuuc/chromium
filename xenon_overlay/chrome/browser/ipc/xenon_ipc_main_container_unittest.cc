// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/base_paths.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "base/base_paths_win.h"
#endif
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/task/execution_fence.h"
#include "base/test/test_future.h"
#include "components/version_info/version_info.h"
#include "gin/converter.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-script.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"
#include "xenon_overlay/common/ipc/xenon_ipc_value_codec.h"
#include "xenon_overlay/services/xenon_node_executor.h"

namespace xenon::ipc {

namespace {

constexpr char kTestMainSource[] = R"JS(
const {app, BrowserWindow, ipcMain, systemPreferences, Menu, nativeImage} = require('electron');
const EventEmitter = require('events');
const net = require('net');
const path = require('node:path');
const os = require('os');
const fs = require('node:fs');
const asyncHooks = require('node:async_hooks');
const ipcContext = new asyncHooks.AsyncLocalStorage();
ipcMain.on('test:native-app-exit', (event, code) => {
  try {
    __xenonExitApp(code);
    event.returnValue = 'requested';
  } catch (error) {
    event.returnValue = error.code || error.name;
  }
});
ipcMain.on('test:unsupported-app-exit', event => {
  const window = new BrowserWindow({show: false});
  let events = 0;
  for (const name of ['before-quit', 'will-quit', 'quit']) {
    app.on(name, () => ++events);
  }
  const codes = [];
  for (const method of ['quit', 'exit']) {
    try { app[method](); } catch (error) { codes.push(error.code); }
  }
  event.returnValue = {codes, events, alive: !window.isDestroyed()};
});
ipcMain.handle('test:async-context-enter', () => {
  ipcContext.enterWith('message');
  return ipcContext.getStore();
});
ipcMain.handle('test:async-context-is-empty', () => ipcContext.getStore() === undefined);
ipcMain.handle('test:async-context-await', async () => {
  const storage = new asyncHooks.AsyncLocalStorage();
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const first = storage.run('first', async () => {
    await gate;
    const before = storage.getStore();
    const nested = await storage.run('nested', async () => {
      await Promise.resolve();
      try { await Promise.reject(new Error('expected')); } catch {}
      return storage.getStore();
    });
    return [before, nested, storage.getStore()].join(',');
  });
  const second = storage.run('second', async () => {
    await Promise.resolve();
    release();
    return storage.getStore();
  });
  const values = await Promise.all([first, second]);
  try { storage.run('throw', () => { throw new Error('expected'); }); } catch {}
  return {first: values[0], second: values[1], restored: storage.getStore() === undefined};
});
ipcMain.handle('test:async-context-tasks', async () => {
  const storage = new asyncHooks.AsyncLocalStorage();
  return storage.run('tasks', async () => {
    const timer = await new Promise(resolve => setTimeout(value =>
        resolve(storage.getStore() + ':' + value), 0, 42));
    const microtask = await new Promise(resolve =>
        queueMicrotask(() => resolve(storage.getStore())));
    const tick = await new Promise(resolve =>
        process.nextTick(() => resolve(storage.getStore())));
    return {timer, microtask, tick, after: storage.getStore()};
  });
});
ipcMain.handle('test:async-context-cleanup', async () => {
  const storage = new asyncHooks.AsyncLocalStorage({defaultValue: 'default'});
  let release;
  const pending = storage.run('old', async () => {
    await new Promise(resolve => { release = resolve; });
    return storage.getStore();
  });
  const stale = storage.run('old', () => asyncHooks.AsyncLocalStorage.snapshot());
  storage.disable();
  const reused = storage.run('new', () => storage.getStore());
  release();
  const pendingValue = await pending;
  const snapshot = storage.run('captured', () => asyncHooks.AsyncLocalStorage.snapshot());
  const later = new asyncHooks.AsyncLocalStorage();
  const isolated = later.run('later', () => snapshot(() =>
      storage.getStore() === 'captured' && later.getStore() === undefined));
  const bound = storage.run('bound', () => asyncHooks.AsyncLocalStorage.bind(function() {
    return this.name + ':' + storage.getStore();
  }));
  return {pendingValue, reused, stale: stale(() => storage.getStore()),
    isolated, bound: bound.call({name: 'receiver'}), root: storage.getStore()};
});
ipcMain.handle('test:async-context-unsupported', () => {
  const calls = [() => asyncHooks.createHook({}), () => asyncHooks.executionAsyncId(),
    () => asyncHooks.triggerAsyncId(), () => asyncHooks.executionAsyncResource(),
    () => new asyncHooks.AsyncResource('fixture').asyncId(),
    () => new asyncHooks.AsyncResource('fixture').triggerAsyncId(),
    () => new asyncHooks.AsyncResource('fixture').emitDestroy()];
  return calls.every(call => {
    try { call(); return false; } catch (error) { return error.code === 'ERR_NOT_SUPPORTED'; }
  });
});
ipcMain.handle('test:async-context-unavailable', () => {
  const storage = new asyncHooks.AsyncLocalStorage();
  storage.disable();
  let callbacks = 0;
  const calls = [() => storage.run('value', () => ++callbacks),
    () => storage.enterWith('value'), () => asyncHooks.AsyncLocalStorage.snapshot(),
    () => asyncHooks.AsyncLocalStorage.bind(() => ++callbacks),
    () => new asyncHooks.AsyncResource('fixture')];
  const errors = calls.map(call => {
    try { call(); return 'no error'; } catch (error) { return error.code; }
  });
  return {errors, callbacks, empty: storage.getStore() === undefined,
    alias: asyncHooks === require('async_hooks')};
});
ipcMain.handle('test:crypto-native', async () => {
  const crypto = require('node:crypto');
  const hashes = Object.fromEntries(['md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512']
    .map(name => [name, crypto.createHash(name).update('a').update('bc').digest('hex')]));
  const bytes = new Uint8Array([77, 255, 0, 128, 97, 77]);
  const binaryHash = crypto.createHash('sha256').update(bytes.subarray(1, 5));
  bytes.fill(0);
  hashes.binary = binaryHash.digest('hex');
  const key = Buffer.from([1, 2, 3]);
  const mac = crypto.createHmac('sha256', key).update('abc');
  key.fill(0);
  hashes.hmac = mac.digest('hex');
  const raw = crypto.createHash('sha256').update('abc').digest();
  hashes.rawBuffer = Buffer.isBuffer(raw) && raw.length === 32;
  const digest = crypto.createHash('sha256').update('abc');
  digest.digest();
  hashes.errors = [];
  for (const fn of [() => digest.digest(), () => digest.update('late'),
      () => crypto.createHash('made-up'), () => crypto.createHmac('sha256'),
      () => crypto.randomBytes(-1), () => crypto.randomBytes(1, null),
      () => crypto.createHash('sha256').update('text', 'unsupported'),
      () => crypto.createHash('sha256').digest('unsupported')]) {
    try { fn(); hashes.errors.push('no error'); } catch (error) { hashes.errors.push(error.code); }
  }
  const previous = Math.random;
  Math.random = () => { throw new Error('insecure randomness used'); };
  try {
    const random = crypto.randomBytes(64);
    hashes.random = Buffer.isBuffer(random) && random.length === 64 && random.some(value => value !== 0);
    hashes.uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID());
    hashes.zero = crypto.randomBytes(0).length;
    let returned = false;
    const pending = new Promise((resolve, reject) => {
      crypto.randomBytes(8, (error, value) => error ? reject(error) : resolve(returned && value.length === 8));
    });
    returned = true;
    hashes.callback = await pending;
  } finally { Math.random = previous; }
  return hashes;
});
ipcMain.handle('test:builtin-capability-contract', async () => {
  const stream = require('node:stream');
  const errors = [];
  const collect = call => {
    try { call(); } catch (error) { errors.push(error.code); }
  };
  for (const name of ['http', 'https']) {
    const http = require(name);
    for (const method of ['createServer']) {
      collect(() => http[method]('https://fixture.invalid'));
    }
  }
  collect(() => new stream.Readable().pipe(new stream.Writable()));
  collect(() => new stream.Writable().write(Buffer.from([0, 255])));
  const writer = new stream.Writable();
  let finishes = 0;
  writer.on('finish', () => ++finishes);
  let returned = false;
  const pending = new Promise(resolve => writer.end('must not disappear', error => {
    errors.push(error.code);
    resolve(returned);
  }));
  returned = true;
  const asynchronous = await pending;
  for (const method of ['chmod', 'chown']) {
    try { await fs.promises[method]('unused', 0o644); }
    catch (error) { errors.push(error.code); }
  }
  function UserlandSink() { stream.call(this); }
  Object.setPrototypeOf(UserlandSink.prototype, stream.prototype);
  UserlandSink.prototype.write = function(value) { this.emit('data', value); };
  const sink = new UserlandSink();
  let kept;
  sink.on('data', value => { kept = value; });
  sink.write('kept');
  return {errors, asynchronous, finishes, kept,
    aliases: stream === require('stream') && stream.Stream === stream &&
        stream.Duplex !== stream.Readable && sink instanceof EventEmitter};
});
ipcMain.handle('test:buffer-base64-native', () => {
  const buffer = Buffer.from([99, 251, 255, 254, 99]);
  const view = buffer.subarray(1, 4);
  return {
    base64: view.toString('base64'),
    base64url: view.toString('base64url'),
    range: buffer.toString('base64', 1, 4),
    paddedView: Buffer.from([0, 255]).subarray(1).toString('base64'),
    decoded: Array.from(Buffer.from('+//+', 'base64')),
    decodedUrl: Array.from(Buffer.from('-__-', 'base64url')),
    tolerant: Array.from(Buffer.from(' _w \n', 'base64')),
    empty: Buffer.alloc(0).toString('base64'),
  };
});
ipcMain.handle('test:buffer-contract', () => {
  const bytes = new Uint8Array([1, 2, 3, 4]);
  const copied = Buffer.from(bytes.subarray(1, 3));
  bytes[1] = 9;
  const shared = Buffer.from(bytes.buffer, 1, 2);
  shared[1] = 8;
  const sliced = copied.slice(1);
  sliced[0] = 7;
  const wide = Buffer.from(new Uint16Array([0x1234, 0x5678]));
  let rangeCode;
  try { Buffer.from(bytes.buffer, 4, 1); }
  catch (error) { rangeCode = error.code; }
  return copied[0] === 2 && copied[1] === 7 && bytes[2] === 8 &&
      shared.buffer === bytes.buffer && shared.byteOffset === 1 &&
      sliced.buffer === copied.buffer && sliced.byteOffset === copied.byteOffset + 1 &&
      Buffer.isBuffer(copied) && Buffer.isBuffer(sliced) && !Buffer.isBuffer(bytes) &&
      wide.length === 2 && wide[0] === 0x34 && wide[1] === 0x78 &&
      Buffer.from(new DataView(bytes.buffer)).length === 0 &&
      rangeCode === 'ERR_BUFFER_OUT_OF_BOUNDS';
});
ipcMain.handle('test:fs-write', async (_event, path) => {
  const bytes = new Uint8Array([99, 0, 255, 128, 65, 99]);
  const pending = fs.promises.writeFile(path, bytes.subarray(1, 5));
  bytes.fill(42);
  await pending;
  return true;
});
ipcMain.handle('test:fs-write-stream', (_event, path) => new Promise(resolve => {
  const writer = fs.createWriteStream(path, {flags: 'a'});
  const events = [];
  writer.on('error', error => events.push(error.code));
  writer.on('finish', () => events.push('finish'));
  writer.on('close', () => resolve({events, bytesWritten: writer.bytesWritten,
    finished: writer.writableFinished, closed: writer.closed, fd: writer.fd}));
  const bytes = new Uint8Array([99, 0, 255, 128, 99]);
  writer.write(bytes.subarray(1, 4), error => events.push(error?.code || 'write'));
  bytes.fill(42);
  writer.end('4142', 'hex', error => events.push(error?.code || 'end'));
}));
ipcMain.handle('test:fs-read', async (_event, path) =>
  Array.from(await fs.promises.readFile(path)));
ipcMain.handle('test:fs-sync', (_event, path) => {
  const bytes = new Uint8Array([99, 0, 255, 128, 65, 99]);
  fs.writeFileSync(path, bytes.subarray(1, 5));
  return Array.from(fs.readFileSync(path));
});
ipcMain.handle('test:fs-callback', (_event, path, write) =>
  new Promise((resolve, reject) => {
    let returned = false;
    const callback = (error, data) => {
      if (error) {
        resolve({returned, code: error.code, path: error.path,
          syscall: error.syscall});
      } else {
        resolve({returned, data: data ? Array.from(data) : null});
      }
    };
    if (write) fs.writeFile(path, new Uint8Array([0, 255, 128, 65]), callback);
    else fs.readFile(path, callback);
    returned = true;
  }));
ipcMain.handle('test:fs-promise-error', async (_event, path) => {
  try { await fs.promises.readFile(path); }
  catch (error) { return {code: error.code, path: error.path, syscall: error.syscall}; }
});
ipcMain.handle('test:fs-operations', async (_event, root) => {
  const nested = path.join(root, 'nested', 'child');
  await fs.promises.mkdir(nested, {recursive: true});
  const source = path.join(nested, 'source');
  const copy = path.join(nested, 'copy');
  const moved = path.join(nested, 'moved');
  await fs.promises.writeFile(source, new Uint8Array([0, 255, 128, 65]));
  await fs.promises.copyFile(source, copy);
  await fs.promises.rename(copy, moved);
  await fs.promises.access(moved);
  const stat = await fs.promises.stat(moved);
  const names = (await fs.promises.readdir(nested)).sort();
  const bytes = Array.from(await fs.promises.readFile(moved));
  await fs.promises.rm(path.join(root, 'nested'), {recursive: true});
  return {size: stat.size, isFile: stat.isFile(), names, bytes};
});
ipcMain.handle('test:fs-unsupported', async () => {
  const errors = [];
  for (const call of [() => fs.openSync('unused', 'r'),
    () => fs.watch('unused'),
    () => fs.promises.open('unused', 'r')]) {
    try { await call(); } catch (error) { errors.push(error.code); }
  }
  await new Promise(resolve => fs.open('unused', 'r', error => {
    errors.push(error.code); resolve();
  }));
  return errors;
});
let persistentCounter = 0;
let pendingNetSocket;
let createdWindow;
let preloadFailure;
let observedContents;
let senderEventCount = 0;
let senderOnceCount = 0;
let deletedProcessIds = [];
let contentsDestroyedCount = 0;
ipcMain.on('test:increment', (_event, amount) => {
  persistentCounter += amount;
});
ipcMain.handle('test:get', () => persistentCounter);
ipcMain.handle('test:async-add', async (_event, left, right) => left + right);
ipcMain.handle('test:structured-clone-echo', (_event, value) => value);
ipcMain.handle('test:throws', () => { throw new Error('sync handler failed'); });
ipcMain.handle('test:rejects', async () => { throw new Error('async handler failed'); });
ipcMain.handle('test:never-settles', () => new Promise(() => {}));
ipcMain.handle('test:event-emitter-contract', () => {
  const emitter = new EventEmitter();
  const order = [];
  function repeated() { order.push('repeated'); }
  function once() { order.push('once'); }
  emitter.on('fixture', repeated);
  emitter.on('fixture', repeated);
  emitter.once('fixture', once);
  emitter.prependListener('fixture', () => order.push('first'));
  const onceIsUnwrapped = emitter.listeners('fixture')[3] === once;
  const rawOnceIsWrapped = emitter.rawListeners('fixture')[3] !== once;
  emitter.removeListener('fixture', repeated);
  const duplicateCount = emitter.listenerCount('fixture', repeated);
  emitter.emit('fixture');
  let unhandledError = false;
  try {
    emitter.emit('error', new Error('fixture error'));
  } catch (error) {
    unhandledError = error.message === 'fixture error';
  }
  emitter.setMaxListeners(0);
  return {
    onceIsUnwrapped,
    rawOnceIsWrapped,
    duplicateCount,
    order: order.join(','),
    remainingCount: emitter.listenerCount('fixture'),
    unhandledError,
    maxListeners: emitter.getMaxListeners(),
  };
});
ipcMain.handle('test:delayed-add', (_event, left, right) =>
    new Promise(resolve => setTimeout(() => resolve(left + right), 10)));
ipcMain.handle('test:next-tick', () => new Promise(resolve => {
  const value = {reading: false};
  let scheduled = false;
  const result = process.nextTick(function(first, missing, last) {
    resolve({
      asynchronous: scheduled,
      returnsUndefined: result === undefined,
      sameObject: first === value,
      missingIsUndefined: missing === undefined,
      argumentCount: arguments.length,
      last,
    });
  }, value, undefined, 42);
  scheduled = true;
}));
ipcMain.handle('test:next-tick-invalid', () =>
    [undefined, null, 1, 'callback', {}].every(callback => {
      try {
        process.nextTick(callback);
        return false;
      } catch (error) {
        return error instanceof TypeError && error.code === 'ERR_INVALID_ARG_TYPE';
      }
    }));
ipcMain.on('test:get-sync', event => {
  event.returnValue = persistentCounter;
});
ipcMain.handle('test:is-ready', () => app.isReady());
const appEventState = {readyCount: 0, activations: [], microtasks: 0, order: []};
app.on('ready', () => {
  ++appEventState.readyCount;
  appEventState.order.push('ready');
});
app.whenReady().then(() => {
  appEventState.order.push('when-ready');
  app.on('activate', function(event, ...args) {
    appEventState.order.push('activate');
    appEventState.activations.push({args, ready: app.isReady(),
      receiverIsApp: this === app, hasEvent: typeof event.preventDefault === 'function'});
    queueMicrotask(() => ++appEventState.microtasks);
  });
});
ipcMain.on('test:app-event-state', event => { event.returnValue = appEventState; });
ipcMain.handle('test:sender', event => ({
  processId: event.processId,
  frameId: event.frameId,
  senderProcessId: event.sender.processId,
  senderFrameId: event.sender.frameId,
}));
ipcMain.handle('test:sender-window', event => {
  const window = BrowserWindow.fromWebContents(event.sender);
  return window ? window.id : 0;
});
ipcMain.handle('test:guest-info', (event, id) => {
  const contents = require('electron').webContents.fromId(id);
  return contents ? {sameSender: contents === event.sender,
    hasOwnerWindow: Boolean(contents.getOwnerBrowserWindow()),
    preferences: contents.getLastWebPreferences()} : null;
});
ipcMain.on('test:reply', (event, value) => {
  event.reply('test:reply-result', value + 1);
});
ipcMain.on('test:net-write-before-connect', () => {
  pendingNetSocket = net.connect('xenon-pending-write-test');
  pendingNetSocket.write('queued-before-connect');
});
app.whenReady().then(() => {
  createdWindow = new BrowserWindow({show: false, webPreferences: {
    preload: path.join(__dirname, 'preload', 'entry.js'),
    contextIsolation: false,
    nodeIntegration: true,
  }});
  createdWindow.webContents.on('preload-error', (event, file, error) => {
    preloadFailure = {file, message: error.message,
      correctSender: event.sender === createdWindow.webContents};
  });
});
ipcMain.on('test:preload-failure', event => { event.returnValue = preloadFailure; });
ipcMain.on('test:mutate-web-preferences', event => {
  const snapshot = createdWindow.webContents.getLastWebPreferences();
  snapshot.preload = 'changed.js';
  event.returnValue = createdWindow.webContents.getLastWebPreferences();
});
ipcMain.handle('test:observe-sender', event => {
  observedContents = event.sender;
  observedContents.on('test:event', () => ++senderEventCount);
  observedContents.once('test:event', () => ++senderOnceCount);
  observedContents.on('render-view-deleted', (_event, processId) => {
    deletedProcessIds.push(processId);
  });
  observedContents.once('destroyed', () => ++contentsDestroyedCount);
  return {
    sameContents: observedContents === createdWindow.webContents,
    sameOwner: observedContents.getOwnerBrowserWindow() === createdWindow,
    sameWindow: BrowserWindow.fromWebContents(observedContents) === createdWindow,
    sameId: require('electron').webContents.fromId(observedContents.id) ===
        observedContents,
  };
});
ipcMain.on('test:emit-sender', event => event.sender.emit('test:event'));
ipcMain.on('test:sender-state', event => {
  event.returnValue = {
    sameSender: event.sender === observedContents,
    senderEventCount, senderOnceCount, deletedProcessIds,
    contentsDestroyedCount, destroyed: observedContents.isDestroyed(),
  };
});
ipcMain.on('test:contents-send', (_event, value) => {
  createdWindow.webContents.send('test:contents-result', value);
});
ipcMain.on('test:sender-send-frame', (event, frameId, value) => {
  event.returnValue = event.sender.sendToFrame(
      frameId, 'test:frame-result', value);
});
ipcMain.handle('test:electron-runtime', () => ({
  joinedPath: path.join('parent', 'child'),
  osRelease: os.release(),
  windowCount: BrowserWindow.getAllWindows().length,
  aeroGlass: systemPreferences.isAeroGlassEnabled(),
}));
ipcMain.handle('test:network-interfaces', () => os.networkInterfaces());
ipcMain.handle('test:os-native-contract', (_event, expected) => {
  const nonempty = value => typeof value === 'string' && value.length > 0;
  const cpus = os.cpus();
  const user = os.userInfo();
  const binaryUser = os.userInfo({encoding: 'buffer'});
  const free = os.freemem();
  const total = os.totalmem();
  const errors = [];
  for (const call of [() => os.availableParallelism(),
                      () => os.getPriority(), () => os.setPriority(0)]) {
    try { call(); errors.push('missing'); }
    catch (error) { errors.push(error.code); }
  }
  // Only booleans and error codes leave this handler; host/user identity is
  // never included in test output or an assertion's actual/expected values.
  return {
    alias: os === require('node:os'),
    metadata: os.platform() === process.platform && os.arch() === process.arch &&
        ['LE', 'BE'].includes(os.endianness()),
    uname: ['type', 'release', 'version', 'machine'].every(
        method => os[method]() === expected[method]),
    hostPaths: [os.hostname(), os.homedir(), os.tmpdir()].every(nonempty),
    cpus: cpus.length === expected.cpuCount && cpus.every(cpu =>
        nonempty(cpu.model) && Number.isFinite(cpu.speed) && cpu.speed >= 0 &&
        ['user', 'nice', 'sys', 'idle', 'irq'].every(key =>
            Number.isFinite(cpu.times[key]) && cpu.times[key] >= 0)),
    memory: total === expected.totalmem && Number.isFinite(free) &&
        free >= 0 && free <= total,
    uptime: Number.isFinite(os.uptime()) && os.uptime() >= 0,
    load: os.loadavg().length === 3 &&
        os.loadavg().every(value => Number.isFinite(value) && value >= 0),
    user: nonempty(user.username) && nonempty(user.homedir) &&
        Number.isInteger(user.uid) && Number.isInteger(user.gid) &&
        (user.shell === null || typeof user.shell === 'string'),
    userBuffers: Buffer.isBuffer(binaryUser.username) &&
        Buffer.isBuffer(binaryUser.homedir) &&
        binaryUser.username.toString('utf8') === user.username &&
        binaryUser.homedir.toString('utf8') === user.homedir &&
        (binaryUser.shell === null || Buffer.isBuffer(binaryUser.shell)),
    errors,
  };
});
ipcMain.handle('test:window-bounds', () => {
  const window = BrowserWindow.getAllWindows()[0];
  window.setBounds({x: 25, y: 40, width: 960, height: 540});
  return window.getBounds();
});
ipcMain.handle('test:web-contents-user-agent', () => {
  const contents = BrowserWindow.getAllWindows()[0].webContents;
  const before = contents.getUserAgent();
  contents.setUserAgent('XenonTest/1.0');
  return {before, after: contents.getUserAgent()};
});
ipcMain.handle('test:path-parse-win32', (_event, targetPath) => path.win32.parse(targetPath));
ipcMain.handle('test:path-parse-posix', (_event, targetPath) => path.posix.parse(targetPath));
ipcMain.handle('test:app-paths', () => ({
  home: app.getPath('home'),
  temp: app.getPath('temp'),
  userData: app.getPath('userData'),
  desktop: app.getPath('desktop'),
  exe: app.getPath('exe'),
  processExe: process.execPath,
  chromeVersion: process.versions.chrome,
}));
let menuClosed = false;
let menuClicked = '';
ipcMain.handle('test:menu-popup', () => {
  menuClosed = false;
  menuClicked = '';
  const menu = Menu.buildFromTemplate([
    {
      type: 'normal',
      label: 'hide',
      icon: nativeImage.createFromPath('C:/icon.png'),
      click: () => { menuClicked = 'hide'; },
    },
    {type: 'separator'},
    {
      type: 'submenu',
      label: 'settings',
      submenu: [{
        type: 'checkbox',
        label: 'always',
        checked: true,
        click: () => { menuClicked = 'always'; },
      }],
    },
  ]);
  menu.once('menu-will-close', () => { menuClosed = true; });
  menu.popup({window: BrowserWindow.getAllWindows()[0]});
  return true;
});
ipcMain.handle('test:menu-state', () => ({menuClosed, menuClicked}));
ipcMain.handle('test:open-dialog-contract', async (_event, mode) => {
  const options = {title: 'Native picker fixture',
    properties: ['openDirectory', 'multiSelections'],
    filters: [{name: 'Fixture', extensions: ['.mp4', 'mkv']}]};
  const dialog = require('electron').dialog;
  try {
    const value = mode === 'sync' ? dialog.showOpenDialogSync(options) :
        await dialog.showOpenDialog(options);
    return {failed: false, value: value === undefined ? null : value,
      isUndefined: value === undefined};
  } catch (error) {
    return {failed: true, code: error.code, name: error.name};
  }
});
)JS";

base::Value Arguments(std::initializer_list<base::Value> values) {
  base::ListValue list;
  for (const base::Value& value : values) {
    list.Append(value.Clone());
  }
  return base::Value(std::move(list));
}

base::Value FileSystemArguments(const std::string& operation,
                                const base::FilePath& path,
                                base::DictValue options = {}) {
  options.Set("operation", operation);
  options.Set("path", path.AsUTF8Unsafe());
  base::ListValue arguments;
  arguments.Append(std::move(options));
  return base::Value(std::move(arguments));
}

class FakeIpcRenderer : public xenon::ipc::mojom::IpcRenderer {
 public:
  mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> BindNewRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  void Dispatch(const std::string& channel, base::Value arguments) override {
    dispatch_future_.SetValue(channel, std::move(arguments));
  }

  base::test::TestFuture<std::string, base::Value>& dispatch_future() {
    return dispatch_future_;
  }

 private:
  mojo::Receiver<xenon::ipc::mojom::IpcRenderer> receiver_{this};
  base::test::TestFuture<std::string, base::Value> dispatch_future_;
};

class XenonIpcMainContainerTest : public gin::V8Test {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void SetUp() override {
    gin::V8Test::SetUp();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    main_script_ = temp_dir_.GetPath().AppendASCII("main.js");
    ASSERT_TRUE(base::WriteFile(main_script_, kTestMainSource));
    base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-main-js",
                                                             main_script_);
    container_ = std::make_unique<XenonIpcMainContainer>();
    XenonIpcMainContainer::WindowHooks window_hooks;
    window_hooks.create = base::BindRepeating(
        [](int width, int height, bool show, bool frame, bool transparent,
           int32_t parent_id, const std::string& title, int32_t* window_id,
           uint64_t* hwnd, std::string* error) {
          *window_id = 1;
          *hwnd = 1;
          return true;
        });
    window_bounds_.Set("x", 0);
    window_bounds_.Set("y", 0);
    window_bounds_.Set("width", 800);
    window_bounds_.Set("height", 600);
    window_hooks.call = base::BindRepeating(
        [](XenonIpcMainContainerTest* self, int32_t window_id,
           const std::string& command, const base::Value& arguments,
           base::Value* result, std::string* error) {
          self->last_window_command_ = command;
          self->last_window_arguments_ = arguments.Clone();
          if (window_id != 1) {
            *error = "unknown test window";
            return false;
          }
          if (command == "set-bounds") {
            self->window_bounds_ = arguments.GetDict().Clone();
            *result = base::Value(self->window_bounds_.Clone());
            return true;
          }
          if (command == "get-bounds") {
            *result = base::Value(self->window_bounds_.Clone());
            return true;
          }
          if (command == "center") {
            return true;
          }
          if (command == "popup-menu") {
            return true;
          }
          if (command == "set-user-agent") {
            const std::string* value = arguments.GetDict().FindString("value");
            if (!value) {
              *error = "missing test user agent";
              return false;
            }
            self->user_agent_ = *value;
            return true;
          }
          if (command == "get-user-agent") {
            *result = base::Value(self->user_agent_);
            return true;
          }
          *error = "unsupported test command";
          return false;
        },
        base::Unretained(this));
    container_->SetWindowHooks(std::move(window_hooks));
    ASSERT_TRUE(container_->Initialize()) << container_->startup_error();
  }

  void TearDown() override {
    container_.reset();
    base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");
    base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-electron-app");
    gin::V8Test::TearDown();
  }

  void ExpectSerializedIntegerArguments(const base::Value& arguments,
                                        int expected) {
    ASSERT_TRUE(IsSerializedIpcValue(arguments));
    v8::HandleScope handle_scope(instance_->isolate());
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(instance_->isolate(), context_);
    std::string error;
    v8::Local<v8::Value> decoded;
    ASSERT_TRUE(
        DeserializeIpcValue(instance_->isolate(), context, arguments, &error)
            .ToLocal(&decoded))
        << error;
    ASSERT_TRUE(decoded->IsArray());
    v8::Local<v8::Value> value;
    ASSERT_TRUE(decoded.As<v8::Array>()->Get(context, 0).ToLocal(&value));
    EXPECT_EQ(expected, value->Int32Value(context).FromMaybe(0));
  }

  base::ScopedTempDir temp_dir_;
  base::FilePath main_script_;
  base::DictValue window_bounds_;
  std::string user_agent_ = "DefaultAgent/1.0";
  std::string last_window_command_;
  base::Value last_window_arguments_;
  std::unique_ptr<XenonIpcMainContainer> container_;
};

TEST_F(XenonIpcMainContainerTest, OpenDialogMissingHostReportsUnsupported) {
  for (const char* mode : {"sync", "async"}) {
    SCOPED_TRACE(mode);
    base::test::TestFuture<mojom::IpcResultPtr> future;
    container_->Invoke("renderer-1", "test:open-dialog-contract",
                       Arguments({base::Value(mode)}), future.GetCallback());
    const auto result = future.Take();
    ASSERT_TRUE(result->success) << result->error;
    const auto& value = result->value.GetDict();
    EXPECT_EQ(true, value.FindBool("failed"));
    ASSERT_TRUE(value.FindString("code"));
    EXPECT_EQ("ERR_NOT_SUPPORTED", *value.FindString("code"));
    ASSERT_TRUE(value.FindString("name"));
    EXPECT_EQ("Error", *value.FindString("name"));
    EXPECT_FALSE(value.contains("value"));
  }
}

TEST_F(XenonIpcMainContainerTest, OpenDialogBrowserFailureNeverLooksCancelled) {
  int calls = 0;
  XenonIpcMainContainer::WindowHooks hooks;
  hooks.show_open_dialog = base::BindRepeating(
      [](int* calls, const std::string& title, bool directory, bool allow_multi,
         const std::vector<std::string>& extensions,
         std::vector<std::string>* paths) {
        ++*calls;
        // A partial reply must not become a successful selection either.
        paths->push_back("C:\\partial\\fixture.mp4");
        return false;
      },
      &calls);
  container_->SetWindowHooks(std::move(hooks));
  for (const char* mode : {"sync", "async"}) {
    SCOPED_TRACE(mode);
    base::test::TestFuture<mojom::IpcResultPtr> future;
    container_->Invoke("renderer-1", "test:open-dialog-contract",
                       Arguments({base::Value(mode)}), future.GetCallback());
    const auto result = future.Take();
    ASSERT_TRUE(result->success) << result->error;
    const auto& value = result->value.GetDict();
    EXPECT_EQ(true, value.FindBool("failed"));
    ASSERT_TRUE(value.FindString("code"));
    EXPECT_EQ("ERR_FAILED", *value.FindString("code"));
    EXPECT_FALSE(value.contains("value"));
  }
  EXPECT_EQ(2, calls);
}

TEST_F(XenonIpcMainContainerTest, OpenDialogPreservesRealCancellation) {
  XenonIpcMainContainer::WindowHooks hooks;
  hooks.show_open_dialog = base::BindRepeating(
      [](const std::string& title, bool directory, bool allow_multi,
         const std::vector<std::string>& extensions,
         std::vector<std::string>* paths) { return true; });
  container_->SetWindowHooks(std::move(hooks));
  for (const char* mode : {"sync", "async"}) {
    SCOPED_TRACE(mode);
    base::test::TestFuture<mojom::IpcResultPtr> future;
    container_->Invoke("renderer-1", "test:open-dialog-contract",
                       Arguments({base::Value(mode)}), future.GetCallback());
    const auto result = future.Take();
    ASSERT_TRUE(result->success) << result->error;
    const auto& value = result->value.GetDict();
    EXPECT_EQ(false, value.FindBool("failed"));
    if (std::string_view(mode) == "sync") {
      EXPECT_EQ(true, value.FindBool("isUndefined"));
      ASSERT_TRUE(value.Find("value"));
      EXPECT_TRUE(value.Find("value")->is_none());
    } else {
      EXPECT_EQ(false, value.FindBool("isUndefined"));
      const auto* reply = value.FindDict("value");
      ASSERT_TRUE(reply);
      EXPECT_EQ(true, reply->FindBool("canceled"));
      const auto* paths = reply->FindList("filePaths");
      ASSERT_TRUE(paths);
      EXPECT_TRUE(paths->empty());
    }
  }
}

TEST_F(XenonIpcMainContainerTest, OpenDialogPreservesSelectionAndOptions) {
  XenonIpcMainContainer::WindowHooks hooks;
  hooks.show_open_dialog = base::BindRepeating(
      [](const std::string& title, bool directory, bool allow_multi,
         const std::vector<std::string>& extensions,
         std::vector<std::string>* paths) {
        EXPECT_EQ("Native picker fixture", title);
        EXPECT_TRUE(directory);
        EXPECT_TRUE(allow_multi);
        EXPECT_EQ((std::vector<std::string>{"mp4", "mkv"}), extensions);
        *paths = {"C:\\media\\first.mp4", "C:\\media\\second.mkv"};
        return true;
      });
  container_->SetWindowHooks(std::move(hooks));
  for (const char* mode : {"sync", "async"}) {
    SCOPED_TRACE(mode);
    base::test::TestFuture<mojom::IpcResultPtr> future;
    container_->Invoke("renderer-1", "test:open-dialog-contract",
                       Arguments({base::Value(mode)}), future.GetCallback());
    const auto result = future.Take();
    ASSERT_TRUE(result->success) << result->error;
    const auto& value = result->value.GetDict();
    EXPECT_EQ(false, value.FindBool("failed"));
    EXPECT_EQ(false, value.FindBool("isUndefined"));
    const base::ListValue* paths;
    if (std::string_view(mode) == "sync") {
      paths = value.FindList("value");
    } else {
      const auto* reply = value.FindDict("value");
      ASSERT_TRUE(reply);
      EXPECT_EQ(false, reply->FindBool("canceled"));
      paths = reply->FindList("filePaths");
    }
    ASSERT_TRUE(paths);
    ASSERT_EQ(2u, paths->size());
    EXPECT_EQ("C:\\media\\first.mp4", (*paths)[0].GetString());
    EXPECT_EQ("C:\\media\\second.mkv", (*paths)[1].GetString());
  }
}

TEST_F(XenonIpcMainContainerTest,
       MainBuiltinCapabilitiesNeverDiscardDataAsSuccess) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:builtin-capability-contract",
                     Arguments({}), future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& values = result->value.GetDict();
  EXPECT_EQ(true, values.FindBool("aliases"));
  EXPECT_EQ(true, values.FindBool("asynchronous"));
  EXPECT_EQ(0, values.FindInt("finishes"));
  ASSERT_TRUE(values.FindString("kept"));
  EXPECT_EQ("kept", *values.FindString("kept"));
  const auto* errors = values.FindList("errors");
  ASSERT_TRUE(errors);
  ASSERT_EQ(7u, errors->size());
  for (const auto& code : *errors) {
    EXPECT_EQ("ERR_NOT_SUPPORTED", code.GetString());
  }
}

#if defined(V8_ENABLE_JAVASCRIPT_PROMISE_HOOKS)
TEST_F(XenonIpcMainContainerTest,
       AsyncLocalStoragePreservesNativeAwaitNestedAndConcurrentScopes) {
  // This uses the actual hosted V8 context and its native SetPromiseHooks,
  // rather than a Node AsyncLocalStorage stand-in or Promise.then patch.
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:async-context-await", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& values = result->value.GetDict();
  ASSERT_TRUE(values.FindString("first"));
  EXPECT_EQ("first,nested,first", *values.FindString("first"));
  ASSERT_TRUE(values.FindString("second"));
  EXPECT_EQ("second", *values.FindString("second"));
  EXPECT_EQ(true, values.FindBool("restored"));
}

TEST_F(XenonIpcMainContainerTest,
       AsyncLocalStoragePropagatesNativeTimersMicrotasksAndNextTick) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:async-context-tasks", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& values = result->value.GetDict();
  ASSERT_TRUE(values.FindString("timer"));
  EXPECT_EQ("tasks:42", *values.FindString("timer"));
  for (const char* name : {"microtask", "tick", "after"}) {
    ASSERT_TRUE(values.FindString(name)) << name;
    EXPECT_EQ("tasks", *values.FindString(name)) << name;
  }
}

TEST_F(XenonIpcMainContainerTest,
       AsyncLocalStorageInvalidatesDisabledStoresAndIsolatesSnapshots) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:async-context-cleanup", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& values = result->value.GetDict();
  for (const auto& [name, expected] :
       {std::pair{"pendingValue", "default"}, std::pair{"reused", "new"},
        std::pair{"stale", "default"}, std::pair{"bound", "receiver:bound"},
        std::pair{"root", "default"}}) {
    ASSERT_TRUE(values.FindString(name)) << name;
    EXPECT_EQ(expected, *values.FindString(name)) << name;
  }
  EXPECT_EQ(true, values.FindBool("isolated"));
}

TEST_F(XenonIpcMainContainerTest,
       AsyncLocalStorageDoesNotLeakEnterWithAcrossIncomingIpc) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> enter;
  container_->Invoke("renderer-1", "test:async-context-enter", Arguments({}),
                     enter.GetCallback());
  auto result = enter.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ("message", result->value.GetString());
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> next;
  container_->Invoke("renderer-1", "test:async-context-is-empty", Arguments({}),
                     next.GetCallback());
  result = next.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.GetBool());
}

#else
TEST_F(XenonIpcMainContainerTest,
       AsyncLocalStorageUnavailableBuildFailsExplicitlyWithoutCrashing) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:async-context-unavailable",
                     Arguments({}), future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& values = result->value.GetDict();
  const auto* errors = values.FindList("errors");
  ASSERT_TRUE(errors);
  ASSERT_EQ(5u, errors->size());
  for (const auto& error : *errors) {
    EXPECT_EQ("ERR_NOT_SUPPORTED", error.GetString());
  }
  EXPECT_EQ(0, values.FindInt("callbacks"));
  EXPECT_EQ(true, values.FindBool("empty"));
  EXPECT_EQ(true, values.FindBool("alias"));
}
#endif  // defined(V8_ENABLE_JAVASCRIPT_PROMISE_HOOKS)

TEST_F(XenonIpcMainContainerTest,
       AsyncHooksUnsupportedLifecycleApisDoNotReturnFakeIds) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:async-context-unsupported",
                     Arguments({}), future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.GetBool());
}

TEST_F(XenonIpcMainContainerTest,
       MainCryptoUsesRealDigestsKeysAndSecureRandomness) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:crypto-native", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& values = result->value.GetDict();
  for (const auto& [name, expected] :
       {std::pair{"md5", "900150983cd24fb0d6963f7d28e17f72"},
        std::pair{"sha1", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        std::pair{"sha224",
                  "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
        std::pair{
            "sha256",
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        std::pair{"sha384",
                  "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff"
                  "5bed8086072ba1e7cc2358baeca134c825a7"},
        std::pair{
            "sha512",
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a21"
            "92992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
        std::pair{
            "binary",
            "7db24bcd8c65b9e0d5c5e147be130fbce7f757295538b0e69888ee7cf545e7ae"},
        std::pair{"hmac",
                  "200fd2f9dada90b91212225a6b5e5975512edffd02503bb98ea612ca904d"
                  "aa24"}}) {
    ASSERT_TRUE(values.FindString(name));
    EXPECT_EQ(expected, *values.FindString(name));
  }
  for (const char* key : {"rawBuffer", "random", "uuid", "callback"}) {
    EXPECT_EQ(true, values.FindBool(key)) << key;
  }
  EXPECT_EQ(0, values.FindInt("zero"));
  ASSERT_TRUE(values.FindList("errors"));
  EXPECT_EQ(
      Arguments(
          {base::Value("ERR_CRYPTO_HASH_FINALIZED"),
           base::Value("ERR_CRYPTO_HASH_FINALIZED"),
           base::Value("ERR_NOT_SUPPORTED"),
           base::Value("ERR_INVALID_ARG_TYPE"), base::Value("ERR_OUT_OF_RANGE"),
           base::Value("ERR_INVALID_ARG_TYPE"),
           base::Value("ERR_NOT_SUPPORTED"), base::Value("ERR_NOT_SUPPORTED")}),
      *values.Find("errors"));
}

TEST_F(XenonIpcMainContainerTest, MainBufferBase64UsesRealV8AndHonorsSubviews) {
  // No Node atob/btoa stubs: the bootstrap executes inside the actual hosted
  // V8 isolate, including the intrinsic encoder's ArrayBufferView handling.
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:buffer-base64-native", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& values = result->value.GetDict();
  for (const auto& [key, expected] :
       {std::pair{"base64", "+//+"}, std::pair{"base64url", "-__-"},
        std::pair{"range", "+//+"}, std::pair{"paddedView", "/w=="},
        std::pair{"empty", ""}}) {
    ASSERT_TRUE(values.FindString(key));
    EXPECT_EQ(expected, *values.FindString(key));
  }
  for (const char* key : {"decoded", "decodedUrl"}) {
    ASSERT_TRUE(values.Find(key));
    EXPECT_EQ(Arguments({base::Value(251), base::Value(255), base::Value(254)}),
              *values.Find(key));
  }
  ASSERT_TRUE(values.Find("tolerant"));
  EXPECT_EQ(Arguments({base::Value(255)}), *values.Find("tolerant"));
}

TEST_F(XenonIpcMainContainerTest, MainBufferContractMatchesNodeInRealV8) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:buffer-contract", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}

TEST_F(XenonIpcMainContainerTest, MainFileWriteRunsOnWorkerAndSnapshotsBytes) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("bytes.bin");
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> write;
  {
    // While the worker is fenced, even pumping the main sequence must not do
    // the filesystem work. A Promise wrapper around synchronous I/O fails this.
    base::ScopedThreadPoolExecutionFence fence;
    container_->Invoke("renderer-1", "test:fs-write",
                       Arguments({base::Value(path.AsUTF8Unsafe())}),
                       write.GetCallback());
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(write.IsReady());
    EXPECT_FALSE(base::PathExists(path));
    base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> responsive;
    container_->Invoke("renderer-1", "test:get", Arguments({}),
                       responsive.GetCallback());
    EXPECT_TRUE(responsive.IsReady());
    EXPECT_TRUE(responsive.Get()->success);
  }
  auto result = write.Take();
  ASSERT_TRUE(result->success) << result->error;
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(std::string("\0\xff\x80"
                        "A",
                        4),
            contents);

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> read;
  container_->Invoke("renderer-1", "test:fs-read",
                     Arguments({base::Value(path.AsUTF8Unsafe())}),
                     read.GetCallback());
  result = read.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(Arguments({base::Value(0), base::Value(255), base::Value(128),
                       base::Value(65)}),
            result->value);
}

TEST_F(XenonIpcMainContainerTest, MainSyncFileApisPreserveBinaryData) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("sync.bin");
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  base::ScopedThreadPoolExecutionFence fence;
  container_->Invoke("renderer-1", "test:fs-sync",
                     Arguments({base::Value(path.AsUTF8Unsafe())}),
                     future.GetCallback());
  ASSERT_TRUE(future.IsReady());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(Arguments({base::Value(0), base::Value(255), base::Value(128),
                       base::Value(65)}),
            result->value);
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(std::string("\0\xff\x80"
                        "A",
                        4),
            contents);
}

TEST_F(XenonIpcMainContainerTest,
       MainFileCallbacksAreAsynchronousAndBinarySafe) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("callback.bin");
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> write;
  {
    base::ScopedThreadPoolExecutionFence fence;
    container_->Invoke(
        "renderer-1", "test:fs-callback",
        Arguments({base::Value(path.AsUTF8Unsafe()), base::Value(true)}),
        write.GetCallback());
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(write.IsReady());
    EXPECT_FALSE(base::PathExists(path));
  }
  auto result = write.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("returned"));

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> read;
  container_->Invoke(
      "renderer-1", "test:fs-callback",
      Arguments({base::Value(path.AsUTF8Unsafe()), base::Value(false)}),
      read.GetCallback());
  result = read.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("returned"));
  ASSERT_TRUE(result->value.GetDict().Find("data"));
  EXPECT_EQ(Arguments({base::Value(0), base::Value(255), base::Value(128),
                       base::Value(65)}),
            *result->value.GetDict().Find("data"));
}

TEST_F(XenonIpcMainContainerTest,
       MainWriteStreamWaitsForWorkerAndPersistsBytes) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("stream.bin");
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  {
    base::ScopedThreadPoolExecutionFence fence;
    container_->Invoke("renderer-1", "test:fs-write-stream",
                       Arguments({base::Value(path.AsUTF8Unsafe())}),
                       future.GetCallback());
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(future.IsReady());
    EXPECT_FALSE(base::PathExists(path));
  }
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& value = result->value.GetDict();
  EXPECT_EQ(5, value.FindInt("bytesWritten"));
  EXPECT_EQ(true, value.FindBool("finished"));
  EXPECT_EQ(true, value.FindBool("closed"));
  ASSERT_TRUE(value.Find("fd"));
  EXPECT_TRUE(value.Find("fd")->is_none());
  ASSERT_TRUE(value.Find("events"));
  EXPECT_EQ(Arguments({base::Value("write"), base::Value("end"),
                       base::Value("finish")}),
            *value.Find("events"));
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(std::string("\0\xff\x80"
                        "AB",
                        5),
            contents);
}

TEST_F(XenonIpcMainContainerTest, MainAsyncFileErrorsHaveCodeAndPath) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("missing.bin");
  for (const char* channel : {"test:fs-callback", "test:fs-promise-error"}) {
    base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
    container_->Invoke("renderer-1", channel,
                       Arguments({base::Value(path.AsUTF8Unsafe())}),
                       future.GetCallback());
    auto result = future.Take();
    ASSERT_TRUE(result->success) << result->error;
    const auto& error = result->value.GetDict();
    ASSERT_TRUE(error.FindString("code"));
    EXPECT_EQ("ENOENT", *error.FindString("code"));
    ASSERT_TRUE(error.FindString("path"));
    EXPECT_EQ(path.AsUTF8Unsafe(), *error.FindString("path"));
    ASSERT_TRUE(error.FindString("syscall"));
    EXPECT_EQ("open", *error.FindString("syscall"));
  }
}

TEST_F(XenonIpcMainContainerTest, MainAsyncFileOperationsUseRealFilesystem) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke(
      "renderer-1", "test:fs-operations",
      Arguments({base::Value(temp_dir_.GetPath().AsUTF8Unsafe())}),
      future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const auto& data = result->value.GetDict();
  EXPECT_EQ(4, data.FindInt("size"));
  EXPECT_EQ(true, data.FindBool("isFile"));
  ASSERT_TRUE(data.Find("names"));
  EXPECT_EQ(Arguments({base::Value("moved"), base::Value("source")}),
            *data.Find("names"));
  ASSERT_TRUE(data.Find("bytes"));
  EXPECT_EQ(Arguments({base::Value(0), base::Value(255), base::Value(128),
                       base::Value(65)}),
            *data.Find("bytes"));
  EXPECT_FALSE(base::PathExists(temp_dir_.GetPath().AppendASCII("nested")));
}

TEST_F(XenonIpcMainContainerTest, MainFileReplyIsSafeAfterRendererRemoval) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("detached.bin");
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  {
    base::ScopedThreadPoolExecutionFence fence;
    container_->Invoke("renderer-1", "test:fs-write",
                       Arguments({base::Value(path.AsUTF8Unsafe())}),
                       future.GetCallback());
    container_->RemoveRenderer("renderer-1");
    ASSERT_TRUE(future.IsReady());
    EXPECT_FALSE(future.Get()->success);
  }
  task_environment_.RunUntilIdle();
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(std::string("\0\xff\x80"
                        "A",
                        4),
            contents);
}

TEST_F(XenonIpcMainContainerTest,
       MainFileReplyIsSafeAfterContainerDestruction) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("shutdown.bin");
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  {
    base::ScopedThreadPoolExecutionFence fence;
    container_->Invoke("renderer-1", "test:fs-write",
                       Arguments({base::Value(path.AsUTF8Unsafe())}),
                       future.GetCallback());
    container_.reset();
    ASSERT_TRUE(future.IsReady());
    EXPECT_FALSE(future.Get()->success);
  }
  // The submitted write may finish, but the reply cannot enter the dead
  // isolate.
  task_environment_.RunUntilIdle();
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(std::string("\0\xff\x80"
                        "A",
                        4),
            contents);
}

TEST_F(XenonIpcMainContainerTest, MainUnsupportedFileApisFailExplicitly) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:fs-unsupported", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_list());
  ASSERT_EQ(4u, result->value.GetList().size());
  for (const auto& code : result->value.GetList()) {
    EXPECT_EQ("ERR_NOT_SUPPORTED", code.GetString());
  }
}

TEST_F(XenonIpcMainContainerTest, MainStateSurvivesIndependentMessages) {
  container_->Send("renderer-1", "test:increment", Arguments({base::Value(2)}));
  container_->Send("renderer-2", "test:increment", Arguments({base::Value(3)}));

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-3", "test:get", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(5, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, BrowserWindowBoundsUseNativeBridge) {
  container_->MarkAppReady();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:window-bounds", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const base::DictValue& bounds = result->value.GetDict();
  EXPECT_EQ(bounds.FindInt("x"), 25);
  EXPECT_EQ(bounds.FindInt("y"), 40);
  EXPECT_EQ(bounds.FindInt("width"), 960);
  EXPECT_EQ(bounds.FindInt("height"), 540);
}

TEST_F(XenonIpcMainContainerTest, MenuPopupSerializesTemplateAndDispatchesClicks) {
  container_->MarkAppReady();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> popup_future;
  container_->Invoke("renderer-1", "test:menu-popup", Arguments({}),
                     popup_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr popup_result = popup_future.Take();
  ASSERT_TRUE(popup_result->success) << popup_result->error;
  EXPECT_EQ("popup-menu", last_window_command_);
  ASSERT_TRUE(last_window_arguments_.is_dict());
  const base::ListValue* items =
      last_window_arguments_.GetDict().FindList("items");
  ASSERT_TRUE(items);
  ASSERT_EQ(items->size(), 3u);
  const base::DictValue& hide = (*items)[0].GetDict();
  EXPECT_EQ("normal", *hide.FindString("type"));
  EXPECT_EQ("hide", *hide.FindString("label"));
  EXPECT_EQ("C:/icon.png", *hide.FindString("icon"));
  EXPECT_EQ("separator", *(*items)[1].GetDict().FindString("type"));
  const base::DictValue& settings = (*items)[2].GetDict();
  EXPECT_EQ("submenu", *settings.FindString("type"));
  EXPECT_EQ("settings", *settings.FindString("label"));
  const base::ListValue* submenu = settings.FindList("submenu");
  ASSERT_TRUE(submenu);
  ASSERT_EQ(submenu->size(), 1u);
  EXPECT_EQ("checkbox", *(*submenu)[0].GetDict().FindString("type"));
  EXPECT_TRUE((*submenu)[0].GetDict().FindBool("checked").value_or(false));

  const int hide_id = hide.FindInt("id").value_or(0);
  ASSERT_GT(hide_id, 0);
  base::DictValue command;
  command.Set("commandId", hide_id);
  container_->DispatchWindowEvent(1, "native-menu-command",
                                  base::Value(std::move(command)));
  container_->DispatchWindowEvent(1, "native-menu-closed", base::Value());

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> state_future;
  container_->Invoke("renderer-1", "test:menu-state", Arguments({}),
                     state_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr state = state_future.Take();
  ASSERT_TRUE(state->success) << state->error;
  ASSERT_TRUE(state->value.is_dict());
  EXPECT_TRUE(state->value.GetDict().FindBool("menuClosed").value_or(false));
  EXPECT_EQ("hide", *state->value.GetDict().FindString("menuClicked"));
}

TEST_F(XenonIpcMainContainerTest, WebContentsUserAgentUsesNativeBridge) {
  container_->MarkAppReady();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:web-contents-user-agent", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& user_agents = result->value.GetDict();
  EXPECT_EQ("DefaultAgent/1.0", *user_agents.FindString("before"));
  EXPECT_EQ("XenonTest/1.0", *user_agents.FindString("after"));
  EXPECT_EQ("XenonTest/1.0", user_agent_);
}

TEST_F(XenonIpcMainContainerTest,
       AppendFileCreatesMissingFileAndPreservesBytes) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("new-append.bin");
  const std::string bytes("\0\xff\x80", 3);
  for (int i = 0; i < 2; ++i) {
    base::DictValue options;
    options.Set("dataBase64", base::Base64Encode(bytes));
    auto result = PerformFileSystemCall(
        FileSystemArguments("append_file", path, std::move(options)));
    ASSERT_TRUE(result->success) << result->error;
  }
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path, &contents));
  EXPECT_EQ(bytes + bytes, contents);
  const base::FilePath empty =
      temp_dir_.GetPath().AppendASCII("empty-append.log");
  base::DictValue empty_options;
  empty_options.Set("dataBase64", "");
  auto opened = PerformFileSystemCall(
      FileSystemArguments("append_file", empty, std::move(empty_options)));
  ASSERT_TRUE(opened->success) << opened->error;
  EXPECT_TRUE(base::PathExists(empty));
}

TEST_F(XenonIpcMainContainerTest, FileSystemCallUsesRealFilesystem) {
  const base::FilePath root = temp_dir_.GetPath().AppendASCII("fs-root");
  const base::FilePath nested = root.AppendASCII("nested");
  const base::FilePath source = nested.AppendASCII("source.bin");
  const base::FilePath destination = nested.AppendASCII("destination.bin");

  auto non_recursive =
      PerformFileSystemCall(FileSystemArguments("mkdir", nested));
  ASSERT_FALSE(non_recursive->success);
  EXPECT_TRUE(non_recursive->error.starts_with("ENOENT:"));

  base::DictValue recursive;
  recursive.Set("recursive", true);
  auto mkdir = PerformFileSystemCall(
      FileSystemArguments("mkdir", nested, std::move(recursive)));
  ASSERT_TRUE(mkdir->success) << mkdir->error;
  EXPECT_TRUE(base::DirectoryExists(nested));

  base::DictValue write_options;
  write_options.Set("dataBase64", base::Base64Encode("real filesystem data"));
  auto write_result = PerformFileSystemCall(
      FileSystemArguments("write_file", source, std::move(write_options)));
  ASSERT_TRUE(write_result->success) << write_result->error;

  auto read = PerformFileSystemCall(
      FileSystemArguments("read_file", source));
  ASSERT_TRUE(read->success) << read->error;
  EXPECT_EQ(read->value.GetString(), base::Base64Encode("real filesystem data"));

  auto stat = PerformFileSystemCall(FileSystemArguments("stat", source));
  ASSERT_TRUE(stat->success) << stat->error;
  EXPECT_TRUE(stat->value.GetDict().FindBool("isFile").value_or(false));

  base::DictValue rename_options;
  rename_options.Set("destination", destination.AsUTF8Unsafe());
  auto rename_result = PerformFileSystemCall(
      FileSystemArguments("rename", source, std::move(rename_options)));
  ASSERT_TRUE(rename_result->success) << rename_result->error;
  EXPECT_FALSE(base::PathExists(source));
  EXPECT_TRUE(base::PathExists(destination));

  auto readdir = PerformFileSystemCall(
      FileSystemArguments("readdir", nested));
  ASSERT_TRUE(readdir->success) << readdir->error;
  ASSERT_EQ(readdir->value.GetList().size(), 1u);
  EXPECT_EQ(readdir->value.GetList().front().GetString(), "destination.bin");

  base::DictValue remove_options;
  remove_options.Set("recursive", true);
  auto remove_result = PerformFileSystemCall(
      FileSystemArguments("rm", root, std::move(remove_options)));
  ASSERT_TRUE(remove_result->success) << remove_result->error;
  EXPECT_FALSE(base::PathExists(root));
}

TEST_F(XenonIpcMainContainerTest, InvokeAwaitsPromise) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:async-add",
                     Arguments({base::Value(20), base::Value(22)}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, ApplicationIpcUsesStructuredCloneEndToEnd) {
  v8::Isolate* isolate = instance_->isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(isolate, context_);

  constexpr char kCreateArguments[] = R"JS((() => {
    const cyclic = {label: 'root'};
    cyclic.self = cyclic;
    return [{
      missing: undefined,
      big: 9007199254740993n,
      date: new Date(1700000000123),
      regexp: /xenon/gi,
      map: new Map([['answer', 42]]),
      set: new Set(['a', 'b']),
      buffer: new Uint8Array([0, 255, 17]).buffer,
      nan: NaN,
      infinity: Infinity,
      cyclic,
    }];
  })())JS";
  v8::Local<v8::Script> create_script;
  ASSERT_TRUE(
      v8::Script::Compile(
          context, gin::StringToV8(isolate, kCreateArguments).As<v8::String>())
          .ToLocal(&create_script));
  v8::Local<v8::Value> arguments;
  ASSERT_TRUE(create_script->Run(context).ToLocal(&arguments));

  base::Value serialized_arguments;
  std::string error;
  ASSERT_TRUE(SerializeIpcValue(isolate, context, arguments,
                                /*rethrow_exception=*/false,
                                &serialized_arguments, &error))
      << error;

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:structured-clone-echo",
                     std::move(serialized_arguments), future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(IsSerializedIpcValue(result->value));

  v8::Local<v8::Value> decoded;
  ASSERT_TRUE(DeserializeIpcValue(isolate, context, result->value, &error)
                  .ToLocal(&decoded))
      << error;
  ASSERT_TRUE(context->Global()
                  ->Set(context,
                        gin::StringToV8(isolate, "ipcResult").As<v8::String>(),
                        decoded)
                  .FromMaybe(false));
  constexpr char kValidateResult[] = R"JS((() => {
    const value = ipcResult;
    return Object.hasOwn(value, 'missing') && value.missing === undefined &&
        value.big === 9007199254740993n &&
        value.date instanceof Date && value.date.getTime() === 1700000000123 &&
        value.regexp instanceof RegExp && value.regexp.source === 'xenon' &&
        value.regexp.flags === 'gi' && value.map instanceof Map &&
        value.map.get('answer') === 42 && value.set instanceof Set &&
        value.set.has('a') && value.set.has('b') &&
        value.buffer instanceof ArrayBuffer &&
        new Uint8Array(value.buffer).join(',') === '0,255,17' &&
        Number.isNaN(value.nan) && value.infinity === Infinity &&
        value.cyclic.self === value.cyclic;
  })())JS";
  v8::Local<v8::Script> validate_script;
  ASSERT_TRUE(
      v8::Script::Compile(
          context, gin::StringToV8(isolate, kValidateResult).As<v8::String>())
          .ToLocal(&validate_script));
  v8::Local<v8::Value> valid;
  ASSERT_TRUE(validate_script->Run(context).ToLocal(&valid));
  EXPECT_TRUE(valid->BooleanValue(isolate));

  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Function> unsupported =
      v8::Function::New(context,
                        [](const v8::FunctionCallbackInfo<v8::Value>&) {})
          .ToLocalChecked();
  base::Value ignored;
  EXPECT_FALSE(SerializeIpcValue(isolate, context, unsupported,
                                 /*rethrow_exception=*/true, &ignored, &error));
  EXPECT_TRUE(try_catch.HasCaught());
  EXPECT_FALSE(error.empty());
}

TEST_F(XenonIpcMainContainerTest, MainEventEmitterMatchesNodeContracts) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:event-emitter-contract", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& contract = result->value.GetDict();
  EXPECT_EQ(true, contract.FindBool("onceIsUnwrapped"));
  EXPECT_EQ(true, contract.FindBool("rawOnceIsWrapped"));
  EXPECT_EQ(1, contract.FindInt("duplicateCount"));
  EXPECT_EQ("first,repeated,once", *contract.FindString("order"));
  EXPECT_EQ(2, contract.FindInt("remainingCount"));
  EXPECT_EQ(true, contract.FindBool("unhandledError"));
  EXPECT_EQ(0, contract.FindInt("maxListeners"));
}

TEST_F(XenonIpcMainContainerTest, InvokeReturnsThrownAndRejectedErrors) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> thrown_future;
  container_->Invoke("renderer-1", "test:throws", Arguments({}),
                     thrown_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr thrown = thrown_future.Take();
  EXPECT_FALSE(thrown->success);
  EXPECT_NE(std::string::npos, thrown->error.find("sync handler failed"));

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> rejected_future;
  container_->Invoke("renderer-1", "test:rejects", Arguments({}),
                     rejected_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr rejected = rejected_future.Take();
  EXPECT_FALSE(rejected->success);
  EXPECT_NE(std::string::npos, rejected->error.find("async handler failed"));
}

TEST_F(XenonIpcMainContainerTest, ShutdownFailsPendingInvoke) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:never-settles", Arguments({}),
                     future.GetCallback());
  EXPECT_FALSE(future.IsReady());

  container_.reset();
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  EXPECT_FALSE(result->success);
  EXPECT_NE(std::string::npos, result->error.find("container stopped"));
}

TEST_F(XenonIpcMainContainerTest, RendererDisconnectFailsPendingInvoke) {
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke(endpoint, "test:never-settles", Arguments({}),
                     future.GetCallback());
  EXPECT_FALSE(future.IsReady());

  container_->RemoveRenderer(endpoint);
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  EXPECT_FALSE(result->success);
  EXPECT_NE(std::string::npos, result->error.find("Renderer disconnected"));
}

TEST_F(XenonIpcMainContainerTest, InvokeAwaitsPromiseResolvedByTimer) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:delayed-add",
                     Arguments({base::Value(20), base::Value(22)}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, NextTickForwardsArgumentsAsynchronously) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:next-tick", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& state = result->value.GetDict();
  EXPECT_EQ(true, state.FindBool("asynchronous"));
  EXPECT_EQ(true, state.FindBool("returnsUndefined"));
  EXPECT_EQ(true, state.FindBool("sameObject"));
  EXPECT_EQ(true, state.FindBool("missingIsUndefined"));
  EXPECT_EQ(3, state.FindInt("argumentCount"));
  EXPECT_EQ(42, state.FindInt("last"));
}

TEST_F(XenonIpcMainContainerTest, NextTickRejectsInvalidCallbackSynchronously) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:next-tick-invalid", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}

TEST_F(XenonIpcMainContainerTest, SendSyncUsesEventReturnValue) {
  container_->Send("renderer-1", "test:increment", Arguments({base::Value(7)}));
  xenon::ipc::mojom::IpcResultPtr result =
      container_->SendSync("renderer-1", "test:get-sync", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(7, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, AppReadyFollowsBrowserLifecycle) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> before_future;
  container_->Invoke("renderer-1", "test:is-ready", Arguments({}),
                     before_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr before = before_future.Take();
  ASSERT_TRUE(before->success) << before->error;
  ASSERT_TRUE(before->value.is_bool());
  EXPECT_FALSE(before->value.GetBool());

  container_->MarkAppReady();

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> after_future;
  container_->Invoke("renderer-1", "test:is-ready", Arguments({}),
                     after_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr after = after_future.Take();
  ASSERT_TRUE(after->success) << after->error;
  ASSERT_TRUE(after->value.is_bool());
  EXPECT_TRUE(after->value.GetBool());
}

TEST_F(XenonIpcMainContainerTest, AppExitWithoutOwnerFailsExplicitly) {
  const auto result = container_->SendSync("renderer-1", "test:native-app-exit",
                                           Arguments({base::Value(0)}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ("ERR_NOT_SUPPORTED", result->value.GetString());
}

TEST_F(XenonIpcMainContainerTest, UnsupportedAppExitDoesNotStartCleanup) {
  const auto result = container_->SendSync(
      "renderer-1", "test:unsupported-app-exit", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  const auto& state = result->value.GetDict();
  EXPECT_EQ(state.FindInt("events"), 0);
  EXPECT_EQ(state.FindBool("alive"), true);
  ASSERT_TRUE(state.FindList("codes"));
  EXPECT_EQ((base::ListValue()
                 .Append("ERR_NOT_SUPPORTED")
                 .Append("ERR_NOT_SUPPORTED")),
            *state.FindList("codes"));
}

TEST_F(XenonIpcMainContainerTest,
       AppExitNotifiesOwnerOnceAfterJavaScriptReturns) {
  std::vector<int> exit_codes;
  container_->SetAppExitHandler(base::BindRepeating(
      [](std::vector<int>* codes, int code) { codes->push_back(code); },
      &exit_codes));
  for (int code : {23, 42}) {
    const auto result = container_->SendSync(
        "renderer-1", "test:native-app-exit", Arguments({base::Value(code)}));
    ASSERT_TRUE(result->success) << result->error;
    EXPECT_EQ("requested", result->value.GetString());
  }
  EXPECT_TRUE(exit_codes.empty());
  task_environment_.RunUntilIdle();
  EXPECT_EQ((std::vector<int>{23}), exit_codes);
}

TEST_F(XenonIpcMainContainerTest, AppExitNotificationDoesNotOutliveContainer) {
  bool notified = false;
  container_->SetAppExitHandler(
      base::BindRepeating([](bool* value, int) { *value = true; }, &notified));
  const auto result = container_->SendSync("renderer-1", "test:native-app-exit",
                                           Arguments({base::Value(0)}));
  ASSERT_TRUE(result->success) << result->error;
  container_.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(notified);
}

TEST_F(XenonIpcMainContainerTest, AppActivateWaitsForReadyAndDrainsMicrotasks) {
  container_->DispatchAppEvent(
      "activate", Arguments({base::Value(false), base::Value("before-ready")}));
  const auto before = container_->SendSync(
      "renderer-1", "test:app-event-state", Arguments({}));
  ASSERT_TRUE(before->success) << before->error;
  EXPECT_EQ(0, before->value.GetDict().FindInt("readyCount"));
  ASSERT_TRUE(before->value.GetDict().FindList("activations"));
  EXPECT_TRUE(before->value.GetDict().FindList("activations")->empty());

  container_->MarkAppReady();
  const auto after = container_->SendSync(
      "renderer-1", "test:app-event-state", Arguments({}));
  ASSERT_TRUE(after->success) << after->error;
  const auto& state = after->value.GetDict();
  EXPECT_EQ(1, state.FindInt("readyCount"));
  EXPECT_EQ(1, state.FindInt("microtasks"));
  ASSERT_TRUE(state.FindList("order"));
  EXPECT_EQ((base::ListValue().Append("ready").Append("when-ready").Append("activate")),
            *state.FindList("order"));
  const auto* activations = state.FindList("activations");
  ASSERT_TRUE(activations);
  ASSERT_EQ(1u, activations->size());
  const auto& activation = (*activations)[0].GetDict();
  EXPECT_EQ(true, activation.FindBool("ready"));
  EXPECT_EQ(true, activation.FindBool("receiverIsApp"));
  EXPECT_EQ(true, activation.FindBool("hasEvent"));
  ASSERT_TRUE(activation.FindList("args"));
  EXPECT_EQ((base::ListValue().Append(false).Append("before-ready")),
            *activation.FindList("args"));
}

TEST_F(XenonIpcMainContainerTest,
       RepeatedAppActivateDoesNotRepeatReadyOrLoseArguments) {
  container_->MarkAppReady();
  container_->DispatchAppEvent("activate", Arguments({base::Value(false)}));
  container_->MarkAppReady();
  container_->DispatchAppEvent("activate", Arguments({base::Value(true)}));
  const auto result = container_->SendSync(
      "renderer-1", "test:app-event-state", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  const auto& state = result->value.GetDict();
  EXPECT_EQ(1, state.FindInt("readyCount"));
  EXPECT_EQ(2, state.FindInt("microtasks"));
  const auto* activations = state.FindList("activations");
  ASSERT_TRUE(activations);
  ASSERT_EQ(2u, activations->size());
  for (size_t i = 0; i < activations->size(); ++i) {
    const auto& activation = (*activations)[i].GetDict();
    EXPECT_EQ(true, activation.FindBool("ready"));
    ASSERT_TRUE(activation.FindList("args"));
    EXPECT_EQ((base::ListValue().Append(i == 1)), *activation.FindList("args"));
  }
}

TEST_F(XenonIpcMainContainerTest, StandardElectronMainBootstrapRunsUnchanged) {
  container_->MarkAppReady();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:electron-runtime", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& runtime = result->value.GetDict();
  const std::string* joined_path = runtime.FindString("joinedPath");
  ASSERT_TRUE(joined_path);
  EXPECT_EQ(base::FilePath(FILE_PATH_LITERAL("parent"))
                .Append(FILE_PATH_LITERAL("child"))
                .AsUTF8Unsafe(),
            *joined_path);
  const std::string* os_release = runtime.FindString("osRelease");
  ASSERT_TRUE(os_release);
  EXPECT_FALSE(os_release->empty());
  EXPECT_EQ(1, runtime.FindInt("windowCount"));
  EXPECT_EQ(false, runtime.FindBool("aeroGlass"));
}

TEST_F(XenonIpcMainContainerTest,
       LoadsElectronPackageMainWithoutSourceChanges) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  const base::FilePath app_dir =
      temp_dir_.GetPath().AppendASCII("electron-app");
  const base::FilePath dist_dir = app_dir.AppendASCII("dist");
  const base::FilePath dependency_dir = app_dir.AppendASCII("node_modules")
                                            .AppendASCII("fixture-dependency")
                                            .AppendASCII("lib");
  ASSERT_TRUE(base::CreateDirectory(dist_dir));
  ASSERT_TRUE(base::CreateDirectory(dependency_dir));
  ASSERT_TRUE(base::WriteFile(app_dir.AppendASCII("package.json"),
                              R"JSON({"main":"dist/main.js"})JSON"));
  ASSERT_TRUE(base::WriteFile(dist_dir.AppendASCII("answer.json"),
                              R"JSON({"value":40})JSON"));
  ASSERT_TRUE(
      base::WriteFile(dependency_dir.DirName().AppendASCII("package.json"),
                      R"JSON({"main":"lib/index.cjs"})JSON"));
  ASSERT_TRUE(base::WriteFile(dependency_dir.AppendASCII("index.cjs"),
                              "module.exports = {increment: 2};"));
  ASSERT_TRUE(base::WriteFile(
      dist_dir.AppendASCII("main.js"),
      "const answer = require('./answer.json').value + "
      "require('fixture-dependency').increment;"
      "const {app, ipcMain} = require('electron');"
      "ipcMain.handle('package:main', () => answer);"
      "ipcMain.handle('package:app-path', () => app.getAppPath());"));
  base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-electron-app",
                                                           app_dir);

  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize()) << container_->startup_error();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "package:main", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> path_future;
  container_->Invoke("renderer-1", "package:app-path", Arguments({}),
                     path_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr path_result = path_future.Take();
  ASSERT_TRUE(path_result->success) << path_result->error;
  ASSERT_TRUE(path_result->value.is_string());
  base::FilePath normalized_app_dir;
  ASSERT_TRUE(base::NormalizeFilePath(app_dir, &normalized_app_dir));
  EXPECT_EQ(normalized_app_dir.AsUTF8Unsafe(), path_result->value.GetString());
}

TEST_F(XenonIpcMainContainerTest, LoadsPackagedMainModuleWithoutDiskFile) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  const base::FilePath app_path =
      temp_dir_.GetPath().AppendASCII("packaged-app");
  const base::FilePath virtual_main = app_path.AppendASCII("main.js");
  XenonIpcMainContainer::EmbeddedMainModule main_module{
      .source =
          "const {app, ipcMain} = require('electron');"
          "ipcMain.handle('packaged:value', () => ({"
          "  answer: 42, appPath: app.getAppPath(), filename: __filename"
          "}));",
      .virtual_path = virtual_main,
      .app_path = app_path,
      .app_name = "Packaged Test",
      .app_version = "1.0.0",
  };
  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize(std::move(main_module)))
      << container_->startup_error();

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "packaged:value", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  EXPECT_EQ(42, result->value.GetDict().FindInt("answer"));
  EXPECT_EQ(app_path.AsUTF8Unsafe(),
            *result->value.GetDict().FindString("appPath"));
  EXPECT_EQ(virtual_main.AsUTF8Unsafe(),
            *result->value.GetDict().FindString("filename"));
  EXPECT_FALSE(base::PathExists(virtual_main));
}

TEST_F(XenonIpcMainContainerTest, UsesExplicitExecutableWithoutRenamingIt) {
  container_.reset();
  const auto executable =
      temp_dir_.GetPath().AppendASCII("actual-launcher.exe");
  ASSERT_TRUE(base::WriteFile(executable, "fixture"));
  XenonIpcMainContainer::EmbeddedMainModule module{
      .source =
          "const {app, ipcMain} = require('electron');"
          "ipcMain.handle('identity', () => ({"
          "exec: process.execPath, argv: process.argv[0],"
          "appExe: app.getPath('exe'), version: app.getVersion()}));",
      .virtual_path = main_script_,
      .app_path = temp_dir_.GetPath(),
      .executable_path = executable,
      .app_name = "DifferentDisplayName",
      .app_version = "3.2.1",
  };
  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize(std::move(module)))
      << container_->startup_error();
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "identity", Arguments({}),
                     future.GetCallback());
  const auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  base::FilePath normalized;
  ASSERT_TRUE(base::NormalizeFilePath(executable, &normalized));
  for (const char* key : {"exec", "argv", "appExe"}) {
    EXPECT_EQ(normalized.AsUTF8Unsafe(),
              *result->value.GetDict().FindString(key));
  }
  EXPECT_EQ("3.2.1", *result->value.GetDict().FindString("version"));
}

TEST_F(XenonIpcMainContainerTest,
       LoadsNativeAddonBesideExplicitExecutable) {
  container_.reset();
  base::FilePath build_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &build_dir));
  const base::FilePath app_dir =
      temp_dir_.GetPath().AppendASCII("resources").AppendASCII("app");
  const base::FilePath main_dir = app_dir.AppendASCII("out");
  const base::FilePath runtime_dir =
      temp_dir_.GetPath().AppendASCII("runtime");
  ASSERT_TRUE(base::CreateDirectory(main_dir));
  ASSERT_TRUE(base::CreateDirectory(runtime_dir));
  const base::FilePath executable =
      runtime_dir.AppendASCII("hosted-app.exe");
  ASSERT_TRUE(base::WriteFile(main_dir.AppendASCII("main.js"), "fixture"));
  ASSERT_TRUE(base::WriteFile(executable, "fixture"));
  ASSERT_TRUE(base::CopyFile(
      build_dir.Append(FILE_PATH_LITERAL("test_addon.node")),
      runtime_dir.Append(FILE_PATH_LITERAL("test_addon.node"))));

  XenonIpcMainContainer::EmbeddedMainModule module{
      .source =
          "const {ipcMain} = require('electron');"
          "const path = require('path');"
          "const native = require(path.join(path.dirname(process.execPath),"
          "  'test_addon.node'));"
          "ipcMain.handle('explicit-native:add',"
          "  (_event, a, b) => native.Add(a, b));",
      .virtual_path = main_dir.AppendASCII("main.js"),
      .app_path = app_dir,
      .executable_path = executable,
  };
  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize(std::move(module)))
      << container_->startup_error();

  base::test::TestFuture<mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "explicit-native:add",
                     Arguments({base::Value(20), base::Value(22)}),
                     future.GetCallback());
  const auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest,
       InvalidExplicitExecutableFailsInitialization) {
  container_.reset();
  XenonIpcMainContainer::EmbeddedMainModule module{
      .source = "module.exports = {};",
      .virtual_path = main_script_,
      .app_path = temp_dir_.GetPath(),
      .executable_path = temp_dir_.GetPath().AppendASCII("missing.exe"),
  };
  container_ = std::make_unique<XenonIpcMainContainer>();
  EXPECT_FALSE(container_->Initialize(std::move(module)));
  EXPECT_NE(std::string::npos, container_->startup_error().find("executable"));
}

TEST_F(XenonIpcMainContainerTest, ExecutableResolutionDoesNotFabricateFiles) {
  base::FilePath actual;
  std::string error;
  ASSERT_TRUE(ResolveAppExecutable({}, &actual, &error)) << error;
  EXPECT_TRUE(base::PathExists(actual));
  EXPECT_FALSE(ResolveAppExecutable(
      base::FilePath(FILE_PATH_LITERAL("relative.exe")), &actual, &error));
  EXPECT_FALSE(ResolveAppExecutable(temp_dir_.GetPath(), &actual, &error));
  const auto missing = temp_dir_.GetPath().AppendASCII("missing.exe");
  EXPECT_FALSE(ResolveAppExecutable(missing, &actual, &error));
  EXPECT_TRUE(GetAppExecutableVersion(missing).empty());
  EXPECT_TRUE(GetAppExecutableVersion(main_script_).empty());
}

TEST_F(XenonIpcMainContainerTest, NetworkInterfacesComeFromTheOperatingSystem) {
  auto native = GetNetworkInterfaces();
  ASSERT_TRUE(native->success) << native->error;
  ASSERT_TRUE(native->value.is_dict());
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:network-interfaces", Arguments({}),
                     future.GetCallback());
  const auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  for (auto [name, addresses] : result->value.GetDict()) {
    EXPECT_FALSE(name.empty());
    ASSERT_TRUE(addresses.is_list());
    for (const auto& address : addresses.GetList()) {
      const auto& entry = address.GetDict();
      ASSERT_TRUE(entry.FindString("address"));
      ASSERT_TRUE(entry.FindString("netmask"));
      ASSERT_TRUE(entry.FindString("mac"));
      EXPECT_EQ(17u, entry.FindString("mac")->size());
      ASSERT_TRUE(entry.FindBool("internal").has_value());
      const auto* family = entry.FindString("family");
      ASSERT_TRUE(family);
      EXPECT_TRUE(*family == "IPv4" || *family == "IPv6");
      const auto* cidr = entry.Find("cidr");
      ASSERT_TRUE(cidr);
      EXPECT_TRUE(cidr->is_none() || cidr->is_string());
      EXPECT_EQ(*family == "IPv6", entry.contains("scopeid"));
    }
  }
}

TEST_F(XenonIpcMainContainerTest, OsModuleUsesNativeQueriesAndPreservesIdentity) {
  base::DictValue expected;
  for (const auto* method : {"type", "release", "version", "machine",
                             "totalmem"}) {
    auto native =
        PerformOsCall(base::Value(base::DictValue().Set("method", method)));
    ASSERT_TRUE(native->success);
    expected.Set(method, std::move(native->value));
  }
  auto cpus =
      PerformOsCall(base::Value(base::DictValue().Set("method", "cpus")));
  ASSERT_TRUE(cpus->success);
  ASSERT_TRUE(cpus->value.is_list());
  expected.Set("cpuCount", static_cast<int>(cpus->value.GetList().size()));
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container_->Invoke(
      "renderer-1", "test:os-native-contract",
      base::Value(base::ListValue().Append(std::move(expected))),
      future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& values = result->value.GetDict();
  for (const auto* key : {"alias", "metadata", "uname", "hostPaths", "cpus",
                          "memory", "uptime", "load", "user", "userBuffers"}) {
    EXPECT_EQ(true, values.FindBool(key)) << key;
  }
  const auto* errors = values.FindList("errors");
  ASSERT_TRUE(errors);
  ASSERT_EQ(3u, errors->size());
  for (const auto& error : *errors) {
    EXPECT_EQ("ERR_NOT_SUPPORTED", error.GetString());
  }
}

#if BUILDFLAG(IS_WIN)
TEST_F(XenonIpcMainContainerTest, ExecutableVersionUsesRealVersionResources) {
  base::FilePath source_root;
  ASSERT_TRUE(
      base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &source_root));
  const auto fixture = source_root.AppendASCII("base/test/data")
                           .AppendASCII("file_version_info_unittest")
                           .AppendASCII("FileVersionInfoTest1.dll");
  // The fixture's numeric version is 1.0.0.1; its localized display string
  // intentionally differs. Runtime identity uses the numeric version.
  EXPECT_EQ("1.0.0.1", GetAppExecutableVersion(fixture));
}
#endif

TEST_F(XenonIpcMainContainerTest, PreservesRendererFrameIdentity) {
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke(endpoint, "test:sender", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& sender = result->value.GetDict();
  EXPECT_EQ(17, sender.FindInt("processId"));
  EXPECT_EQ(23, sender.FindInt("frameId"));
  EXPECT_EQ(17, sender.FindInt("senderProcessId"));
  EXPECT_EQ(23, sender.FindInt("senderFrameId"));
}

TEST_F(XenonIpcMainContainerTest, ResolvesBrowserWindowFromRendererSender) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke(endpoint, "test:sender-window", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(1, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, MainCanReplyToOriginatingRenderer) {
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23);
  container_->Send(endpoint, "test:reply", Arguments({base::Value(41)}));

  EXPECT_EQ("test:reply-result", renderer.dispatch_future().Get<0>());
  const base::Value& arguments = renderer.dispatch_future().Get<1>();
  ExpectSerializedIntegerArguments(arguments, 42);
}

TEST_F(XenonIpcMainContainerTest, PreloadPreferencesBelongToSenderWindow) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  auto result = container_->SendSync(
      endpoint, "__xenon:renderer-web-preferences", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  const auto expected_preload = temp_dir_.GetPath()
                                    .AppendASCII("preload")
                                    .AppendASCII("entry.js")
                                    .AsUTF8Unsafe();
  ASSERT_TRUE(result->value.is_dict());
  ASSERT_TRUE(result->value.GetDict().FindString("preload"));
  EXPECT_STRCASEEQ(expected_preload.c_str(),
                   result->value.GetDict().FindString("preload")->c_str());
  EXPECT_EQ(false, result->value.GetDict().FindBool("contextIsolation"));
  EXPECT_EQ(true, result->value.GetDict().FindBool("nodeIntegration"));

  result = container_->SendSync(endpoint, "test:mutate-web-preferences",
                                Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.GetDict().FindString("preload"));
  EXPECT_STRCASEEQ(expected_preload.c_str(),
                   result->value.GetDict().FindString("preload")->c_str());

  FakeIpcRenderer unattached_renderer;
  const std::string unattached =
      container_->AddRenderer(unattached_renderer.BindNewRemote(), 18, 24);
  result = container_->SendSync(unattached, "__xenon:renderer-web-preferences",
                                Arguments({base::Value(1)}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.is_none());
}

TEST_F(XenonIpcMainContainerTest,
       GuestIdentitySurvivesNavigationAndRoutesToHost) {
  container_->MarkAppReady();
  FakeIpcRenderer owner, guest, replacement, unrelated;
  const auto owner_id =
      container_->AddRenderer(owner.BindNewRemote(), 17, 23, 1);
  const auto unrelated_id =
      container_->AddRenderer(unrelated.BindNewRemote(), 19, 25, 2);
  base::DictValue preferences;
  preferences.Set("preload", "C:\\app\\renderer.asar\\preload.js");
  preferences.Set("contextIsolation", false);
  preferences.Set("nodeIntegration", false);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> registered;
  container_->Invoke(
      owner_id, "__xenon:register-guest",
      Arguments({base::Value(-1), base::Value(preferences.Clone())}),
      registered.GetCallback());
  auto result = registered.Take();
  ASSERT_TRUE(result->success) << result->error;
  const int public_id = result->value.GetInt();
  const auto guest_endpoint =
      container_->AddRenderer(guest.BindNewRemote(), 18, 24, -1);
  auto prefs =
      container_->SendSync(guest_endpoint, "__xenon:renderer-web-preferences",
                           Arguments({base::Value(true)}));
  ASSERT_TRUE(prefs->success) << prefs->error;
  EXPECT_EQ(prefs->value.GetDict(), preferences);
  container_->RemoveRenderer(guest_endpoint);
  const auto next_endpoint =
      container_->AddRenderer(replacement.BindNewRemote(), 20, 26, -1);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> state;
  container_->Invoke(next_endpoint, "test:guest-info",
                     Arguments({base::Value(public_id)}), state.GetCallback());
  result = state.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(result->value.GetDict().FindBool("sameSender"), true);
  EXPECT_EQ(result->value.GetDict().FindBool("hasOwnerWindow"), false);

  container_->Send(next_endpoint, "__xenon:send-to-host",
                   Arguments({base::Value("fixture"),
                              base::Value(base::ListValue().Append(42))}));
  auto [channel, args] = owner.dispatch_future().Take();
  EXPECT_EQ(channel, "__xenon:guest-event");
  EXPECT_EQ(args.GetList()[0].GetInt(), public_id);
  EXPECT_EQ(args.GetList()[1].GetString(), "ipc-message");

  container_->Invoke(
      unrelated_id, "__xenon:guest-call",
      Arguments(
          {base::Value(public_id), base::Value("loadURL"),
           base::Value(base::ListValue().Append("https://example.test/"))}),
      state.GetCallback());
  EXPECT_FALSE(state.Take()->success);
  container_->DispatchWindowEvent(-1, "guest-destroyed", base::Value());
  auto destroyed_event = owner.dispatch_future().Take();
  EXPECT_EQ(std::get<1>(destroyed_event).GetList()[1].GetString(), "destroyed");
  container_->Invoke(owner_id, "test:guest-info",
                     Arguments({base::Value(public_id)}), state.GetCallback());
  result = state.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.is_none());
}

TEST_F(XenonIpcMainContainerTest, PreloadErrorIsDeliveredToOwningWebContents) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  container_->Send(
      endpoint, "__xenon:preload-error",
      Arguments({base::Value("entry.js"), base::Value("fixture failure")}));
  auto result =
      container_->SendSync(endpoint, "test:preload-failure", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  EXPECT_EQ("entry.js", *result->value.GetDict().FindString("file"));
  EXPECT_EQ("fixture failure", *result->value.GetDict().FindString("message"));
  EXPECT_EQ(true, result->value.GetDict().FindBool("correctSender"));
}

TEST_F(XenonIpcMainContainerTest, SenderUsesStableWindowWebContents) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke(endpoint, "test:observe-sender", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  for (const char* key :
       {"sameContents", "sameOwner", "sameWindow", "sameId"}) {
    EXPECT_EQ(true, result->value.GetDict().FindBool(key)) << key;
  }
  container_->Send(endpoint, "test:emit-sender", Arguments({}));
  container_->Send(endpoint, "test:emit-sender", Arguments({}));
  result = container_->SendSync(endpoint, "test:sender-state", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("sameSender"));
  EXPECT_EQ(2, result->value.GetDict().FindInt("senderEventCount"));
  EXPECT_EQ(1, result->value.GetDict().FindInt("senderOnceCount"));

  container_->RemoveRenderer(endpoint);
  FakeIpcRenderer next_renderer;
  const std::string next_endpoint =
      container_->AddRenderer(next_renderer.BindNewRemote(), 18, 24, 1);
  result = container_->SendSync(next_endpoint, "test:sender-state", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("sameSender"));
  EXPECT_EQ(false, result->value.GetDict().FindBool("destroyed"));
  const auto* deleted = result->value.GetDict().FindList("deletedProcessIds");
  ASSERT_TRUE(deleted);
  ASSERT_EQ(1u, deleted->size());
  EXPECT_EQ(17, (*deleted)[0].GetInt());

  container_->DispatchWindowEvent(1, "closed", base::Value());
  container_->DispatchWindowEvent(1, "closed", base::Value());
  result = container_->SendSync("observer", "test:sender-state", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("destroyed"));
  EXPECT_EQ(1, result->value.GetDict().FindInt("contentsDestroyedCount"));
}

TEST_F(XenonIpcMainContainerTest, WebContentsSendsOnlyToItsRegisteredRenderer) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  FakeIpcRenderer unrelated_renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  const std::string unrelated_endpoint =
      container_->AddRenderer(unrelated_renderer.BindNewRemote(), 18, 24, 2);
  // No incoming IPC from the destination is needed to establish routing.
  container_->Send(unrelated_endpoint, "test:contents-send",
                   Arguments({base::Value(42)}));
  auto [channel, arguments] = renderer.dispatch_future().Take();
  EXPECT_EQ("test:contents-result", channel);
  ExpectSerializedIntegerArguments(arguments, 42);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(unrelated_renderer.dispatch_future().IsReady());

  auto result = container_->SendSync(
      endpoint, "test:sender-send-frame",
      Arguments({base::Value(23), base::Value(43)}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.GetBool());
  auto [frame_channel, frame_arguments] = renderer.dispatch_future().Take();
  EXPECT_EQ("test:frame-result", frame_channel);
  ExpectSerializedIntegerArguments(frame_arguments, 43);

  // A frame in a different WebContents must not receive this message.
  result = container_->SendSync(
      endpoint, "test:sender-send-frame",
      Arguments({Arguments({base::Value(18), base::Value(24)}), base::Value(44)}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_FALSE(result->value.GetBool());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(renderer.dispatch_future().IsReady());
  EXPECT_FALSE(unrelated_renderer.dispatch_future().IsReady());
}

TEST_F(XenonIpcMainContainerTest, NetSocketBuffersWritesUntilConnected) {
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23);
  base::test::TestFuture<const std::string&, base::Value> native_send;
  container_->SetNetPipeSender(native_send.GetRepeatingCallback());

  container_->Send(endpoint, "test:net-write-before-connect", Arguments({}));
  auto [connect_channel, connect_arguments] = native_send.Take();
  ASSERT_EQ("__xenon:net:connect", connect_channel);
  ASSERT_TRUE(connect_arguments.is_dict());
  const base::DictValue& connect = connect_arguments.GetDict();
  const std::string* from_id = connect.FindString("fromId");
  ASSERT_TRUE(from_id);

  base::DictValue connected;
  connected.Set("toId", *from_id);
  connected.Set("peerId", "renderer-test-peer");
  container_->Send("@main", "__xenon:net:connected",
                   Arguments({base::Value(std::move(connected))}));

  auto [data_channel, data_arguments] = native_send.Take();
  ASSERT_EQ("__xenon:net:data", data_channel);
  ASSERT_TRUE(data_arguments.is_dict());
  const base::DictValue& data = data_arguments.GetDict();
  EXPECT_EQ("renderer-test-peer", *data.FindString("toId"));
  const base::DictValue* wire = data.FindDict("wire");
  ASSERT_TRUE(wire);
  const std::string* encoded = wire->FindString("d");
  ASSERT_TRUE(encoded);
  EXPECT_FALSE(encoded->empty());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(renderer.dispatch_future().IsReady());
  container_->SetNetPipeSender({});
}

TEST_F(XenonIpcMainContainerTest, CommonJsCanRequireNodeApiAddon) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  base::FilePath executable_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  const base::FilePath app_dir = temp_dir_.GetPath().AppendASCII("native-app");
  ASSERT_TRUE(base::CreateDirectory(app_dir));
  ASSERT_TRUE(base::CopyFile(
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node")),
      app_dir.Append(FILE_PATH_LITERAL("test_addon.node"))));
  ASSERT_TRUE(base::WriteFile(app_dir.AppendASCII("package.json"),
                              R"JSON({"main":"main.js"})JSON"));
  ASSERT_TRUE(base::WriteFile(
      app_dir.AppendASCII("main.js"),
      "const {ipcMain} = require('electron');"
      "const native = require('./test_addon.node');"
      "ipcMain.handle('native:add', (_event, a, b) => native.Add(a, b));"));
  base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-electron-app",
                                                           app_dir);

  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize()) << container_->startup_error();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "native:add",
                     Arguments({base::Value(20), base::Value(22)}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, NodeAddonCacheIsIsolatedByCanonicalPath) {
  base::FilePath executable_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  const base::FilePath executable_addon =
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node"));

  base::ScopedTempDir addon_dir;
  ASSERT_TRUE(addon_dir.CreateUniqueTempDirUnderPath(executable_dir));
  const base::FilePath nested_addon =
      addon_dir.GetPath().Append(FILE_PATH_LITERAL("test_addon.node"));
  ASSERT_TRUE(base::CopyFile(executable_addon, nested_addon));

  XenonNodeExecutor executor;
  std::string load_error;
  ASSERT_TRUE(executor.LoadAddonFromCurrentThread(nested_addon.AsUTF8Unsafe(),
                                                  &load_error))
      << load_error;
  EXPECT_TRUE(executor.HasModule(nested_addon.AsUTF8Unsafe()));
  EXPECT_FALSE(executor.HasModule(executable_addon.AsUTF8Unsafe()));
}

#if BUILDFLAG(IS_WIN)
TEST_F(XenonIpcMainContainerTest,
       NodeAddonRedirectsHostLibrariesToRuntimeDirectory) {
  base::FilePath executable_dir;
  base::FilePath system_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  ASSERT_TRUE(base::PathService::Get(base::DIR_SYSTEM, &system_dir));

  base::ScopedTempDir runtime_dir;
  ASSERT_TRUE(runtime_dir.CreateUniqueTempDir());
  const base::FilePath relative_library =
      base::FilePath(FILE_PATH_LITERAL("private-sdk"))
          .Append(FILE_PATH_LITERAL("version.dll"));
  const base::FilePath hosted_library =
      runtime_dir.GetPath().Append(relative_library);
  ASSERT_TRUE(base::CreateDirectory(hosted_library.DirName()));
  ASSERT_TRUE(base::CopyFile(
      system_dir.Append(FILE_PATH_LITERAL("version.dll")), hosted_library));

  const base::FilePath host_relative_request =
      executable_dir.Append(relative_library);
  ASSERT_FALSE(base::PathExists(host_relative_request));

  const base::FilePath addon_path =
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node"));
  XenonNodeExecutor executor;
  executor.SetRuntimeDirectory(runtime_dir.GetPath());
  base::ListValue arguments;
  arguments.Append(host_relative_request.AsUTF8Unsafe());
  base::Value result;
  std::string error;
  ASSERT_TRUE(executor.InvokeExportFromCurrentThread(
      addon_path.AsUTF8Unsafe(), "CanLoadLibrary",
      base::Value(std::move(arguments)), &result, &error))
      << error;
  ASSERT_TRUE(result.is_bool());
  EXPECT_TRUE(result.GetBool());
}
#endif

TEST_F(XenonIpcMainContainerTest, NodeAddonResolvesCallbacksNestedInObjects) {
  base::FilePath executable_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  const base::FilePath addon_path =
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node"));

  XenonNodeExecutor executor;
  std::string load_error;
  ASSERT_TRUE(executor.LoadAddonFromCurrentThread(addon_path.AsUTF8Unsafe(),
                                                  &load_error))
      << load_error;

  int32_t received_callback_id = 0;
  std::vector<base::Value> received_args;
  executor.SetCallbackHandlers(
      base::BindRepeating(
          [](int32_t* received_id, std::vector<base::Value>* received_args,
             int32_t client_id, int32_t callback_id,
             std::vector<base::Value> args, base::Value) {
            EXPECT_EQ(17, client_id);
            *received_id = callback_id;
            *received_args = std::move(args);
          },
          &received_callback_id, &received_args),
      base::BindRepeating([](int32_t, int32_t) {}));

  base::DictValue callback_wire;
  callback_wire.Set("__xenon_node_wire_type__", "callback");
  callback_wire.Set("callback_id", 41);
  base::DictValue handler;
  handler.Set("onValue", std::move(callback_wire));
  base::ListValue handlers;
  handlers.Append(std::move(handler));
  base::DictValue options;
  options.Set("handlers", std::move(handlers));

  auto argument = xenon::mojom::NodeInvokeArg::New();
  argument->is_callback = false;
  argument->callback_id = 0;
  argument->value = base::Value(std::move(options));
  std::vector<xenon::mojom::NodeInvokeArgPtr> arguments;
  arguments.push_back(std::move(argument));

  base::test::TestFuture<
      bool, base::Value, std::vector<xenon::mojom::NodeCallbackResultPtr>,
      std::string>
      invoke_future;
  executor.InvokeFunction(addon_path.AsUTF8Unsafe(), "InvokeNestedCallback",
                          /*client_id=*/17, std::move(arguments),
                          base::BindOnce(
                              [](decltype(invoke_future)* future, bool success,
                                 base::Value result,
                                 std::vector<
                                     xenon::mojom::NodeCallbackResultPtr>
                                     callback_results,
                                 const std::string& error) {
                                future->SetValue(
                                    success, std::move(result),
                                    std::move(callback_results), error);
                              },
                              &invoke_future));
  auto [success, result, callback_results, invoke_error] = invoke_future.Take();
  EXPECT_TRUE(success) << invoke_error;
  EXPECT_EQ(41, received_callback_id);
  ASSERT_EQ(1u, received_args.size());
  ASSERT_TRUE(received_args[0].is_string());
  EXPECT_EQ("nested callback payload", received_args[0].GetString());
}

TEST_F(XenonIpcMainContainerTest, NodeAddonReturnsCallableFunctionHandles) {
  base::FilePath executable_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  const base::FilePath addon_path =
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node"));

  XenonNodeExecutor executor;
  std::string load_error;
  ASSERT_TRUE(
      executor.LoadAddonFromCurrentThread(addon_path.AsUTF8Unsafe(), &load_error))
      << load_error;

  std::vector<base::Value> received_args;
  executor.SetCallbackHandlers(
      base::BindRepeating(
          [](std::vector<base::Value>* received_args, int32_t client_id,
             int32_t callback_id, std::vector<base::Value> args, base::Value) {
            EXPECT_EQ(17, client_id);
            EXPECT_EQ(42, callback_id);
            *received_args = std::move(args);
          },
          &received_args),
      base::BindRepeating([](int32_t, int32_t) {}));

  auto callback_argument = xenon::mojom::NodeInvokeArg::New();
  callback_argument->is_callback = true;
  callback_argument->callback_id = 42;
  std::vector<xenon::mojom::NodeInvokeArgPtr> callback_arguments;
  callback_arguments.push_back(std::move(callback_argument));

  base::test::TestFuture<
      bool, base::Value, std::vector<xenon::mojom::NodeCallbackResultPtr>,
      std::string>
      invoke_future;
  executor.InvokeFunction(
      addon_path.AsUTF8Unsafe(), "InvokeCallbackWithFunction",
      /*client_id=*/17, std::move(callback_arguments),
      base::BindOnce(
          [](decltype(invoke_future)* future, bool success, base::Value result,
             std::vector<xenon::mojom::NodeCallbackResultPtr> callback_results,
             const std::string& error) {
            future->SetValue(success, std::move(result),
                             std::move(callback_results), error);
          },
          &invoke_future));
  auto [success, result, callback_results, invoke_error] = invoke_future.Take();
  ASSERT_TRUE(success) << invoke_error;
  ASSERT_EQ(1u, received_args.size());
  ASSERT_TRUE(received_args[0].is_dict());
  const base::DictValue& function_wire = received_args[0].GetDict();
  EXPECT_EQ("native_function",
            *function_wire.FindString("__xenon_node_wire_type__"));
  const std::optional<int> instance_id = function_wire.FindInt("instance_id");
  ASSERT_TRUE(instance_id);

  base::DictValue undefined_wire;
  undefined_wire.Set("__xenon_node_wire_type__", "undefined");
  auto this_argument = xenon::mojom::NodeInvokeArg::New();
  this_argument->value = base::Value(std::move(undefined_wire));
  auto value_argument = xenon::mojom::NodeInvokeArg::New();
  value_argument->value = base::Value(41);
  std::vector<xenon::mojom::NodeInvokeArgPtr> call_arguments;
  call_arguments.push_back(std::move(this_argument));
  call_arguments.push_back(std::move(value_argument));

  base::test::TestFuture<
      bool, base::Value, std::vector<xenon::mojom::NodeCallbackResultPtr>,
      std::string>
      call_future;
  executor.InvokeInstance(addon_path.AsUTF8Unsafe(), *instance_id, "call",
                          /*client_id=*/17, std::move(call_arguments),
                          base::BindOnce(
                              [](decltype(call_future)* future, bool success,
                                 base::Value result,
                                 std::vector<
                                     xenon::mojom::NodeCallbackResultPtr>
                                     callback_results,
                                 const std::string& error) {
                                future->SetValue(
                                    success, std::move(result),
                                    std::move(callback_results), error);
                              },
                              &call_future));
  auto [call_success, call_result, call_callbacks, call_error] =
      call_future.Take();
  ASSERT_TRUE(call_success) << call_error;
  ASSERT_TRUE(call_result.is_int());
  EXPECT_EQ(42, call_result.GetInt());
}

TEST_F(XenonIpcMainContainerTest, StandardPathModuleParsesRootsAndPosixWin32) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> win32_future;
  container_->Invoke("renderer-1", "test:path-parse-win32",
                     Arguments({base::Value("C:\\Windows\\System32\\cmd.exe")}),
                     win32_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr win32_result = win32_future.Take();
  ASSERT_TRUE(win32_result->success) << win32_result->error;
  ASSERT_TRUE(win32_result->value.is_dict());
  const base::DictValue& win32_dict = win32_result->value.GetDict();
  EXPECT_EQ("C:\\", *win32_dict.FindString("root"));
  EXPECT_EQ("C:\\Windows\\System32", *win32_dict.FindString("dir"));
  EXPECT_EQ("cmd.exe", *win32_dict.FindString("base"));
  EXPECT_EQ(".exe", *win32_dict.FindString("ext"));
  EXPECT_EQ("cmd", *win32_dict.FindString("name"));

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> posix_future;
  container_->Invoke("renderer-1", "test:path-parse-posix",
                     Arguments({base::Value("/usr/local/bin/node")}),
                     posix_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr posix_result = posix_future.Take();
  ASSERT_TRUE(posix_result->success) << posix_result->error;
  ASSERT_TRUE(posix_result->value.is_dict());
  const base::DictValue& posix_dict = posix_result->value.GetDict();
  EXPECT_EQ("/", *posix_dict.FindString("root"));
  EXPECT_EQ("/usr/local/bin", *posix_dict.FindString("dir"));
  EXPECT_EQ("node", *posix_dict.FindString("base"));
  EXPECT_EQ("", *posix_dict.FindString("ext"));
  EXPECT_EQ("node", *posix_dict.FindString("name"));
}

TEST_F(XenonIpcMainContainerTest, AppGetPathAndDynamicVersions) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:app-paths", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& dict = result->value.GetDict();

  const std::string* home = dict.FindString("home");
  ASSERT_TRUE(home);
  EXPECT_FALSE(home->empty());

  const std::string* temp = dict.FindString("temp");
  ASSERT_TRUE(temp);
  EXPECT_FALSE(temp->empty());

  const std::string* userData = dict.FindString("userData");
  ASSERT_TRUE(userData);
  EXPECT_FALSE(userData->empty());

  const std::string* desktop = dict.FindString("desktop");
  ASSERT_TRUE(desktop);
  EXPECT_FALSE(desktop->empty());

  const std::string* exe = dict.FindString("exe");
  ASSERT_TRUE(exe);
  EXPECT_FALSE(exe->empty());
  EXPECT_EQ(*exe, *dict.FindString("processExe"));
  EXPECT_TRUE(base::PathExists(base::FilePath::FromUTF8Unsafe(*exe)));

  const std::string* chromeVersion = dict.FindString("chromeVersion");
  ASSERT_TRUE(chromeVersion);
  EXPECT_EQ(std::string(version_info::GetVersionNumber()), *chromeVersion);
}

TEST_F(XenonIpcMainContainerTest, InitializationFailsForInvalidMainScript) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  const base::FilePath invalid_main =
      temp_dir_.GetPath().AppendASCII("invalid-main.js");
  ASSERT_TRUE(base::WriteFile(invalid_main, "function broken("));
  base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-main-js",
                                                           invalid_main);

  container_ = std::make_unique<XenonIpcMainContainer>();
  EXPECT_FALSE(container_->Initialize());
  EXPECT_FALSE(container_->is_initialized());
  EXPECT_FALSE(container_->startup_error().empty());
}

TEST_F(XenonIpcMainContainerTest, PackageMainCanPointToItsOwnDirectory) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  const base::FilePath app_dir =
      temp_dir_.GetPath().AppendASCII("self-main-app");
  ASSERT_TRUE(base::CreateDirectory(app_dir));
  ASSERT_TRUE(base::WriteFile(app_dir.AppendASCII("package.json"),
                              R"JSON({"main":"."})JSON"));
  ASSERT_TRUE(base::WriteFile(app_dir.AppendASCII("index.js"),
                              "const {ipcMain} = require('electron');"
                              "ipcMain.handle('self-main:value', () => 42);"));
  base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-electron-app",
                                                           app_dir);

  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize()) << container_->startup_error();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "self-main:value", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(42, result->value.GetInt());
}

}  // namespace

}  // namespace xenon::ipc

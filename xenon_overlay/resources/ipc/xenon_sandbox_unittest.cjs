'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

function fixture() {
  const context = vm.createContext({});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'renderer/sandbox.js'), 'utf8'), context);
  vm.runInContext(`
    class Emitter {
      constructor() { this.listeners = new Map(); }
      on(name, callback) {
        if (!this.listeners.has(name)) this.listeners.set(name, []);
        this.listeners.get(name).push(callback); return this;
      }
      emit(name, ...args) { for (const f of this.listeners.get(name) || []) f(...args); }
    }
    const buffer = {from: value => ({value})};
    const sent = [];
    const timers = {setImmediate: callback => { callback(); return 10; }, clearImmediate() {}};
    const events = {EventEmitter: Emitter};
    const sourceProcess = {sandboxed: true, arch:'x64', platform:'win32',
      type:'renderer', contextId:'context-1', contextIsolated:true,
      argv:['host.exe'], env:{EXAMPLE:'value'}, versions:{electron:'39'},
      _linkedBinding() { throw Error('privileged binding'); },
      cwd() { throw Error('privileged cwd'); },
      nextTick() { throw Error('privileged nextTick'); },
      getSystemVersion: () => 'native-version'};
    const allElectron = {
      ipcRenderer: {send: (...args) => sent.push(args)},
      contextBridge: {exposeInMainWorld: (name, value) => { globalThis[name] = value; }},
      crashReporter: {}, nativeImage: {}, webFrame: {},
      app: {}, remote: {}, clipboard: {}, shell: {}, autoUpdater: {},
    };
    Object.assign(globalThis, {require: () => { throw Error('unrestricted require'); },
      process: sourceProcess, electron: allElectron, xenonIpcRenderer: {},
      __xenonPaths: {}, __xenonElectronRequire: {}, __xenonElectronIpc: {},
      __filename: 'private.js', __dirname: '/private', global: globalThis, Buffer: buffer});
    const loaded = [];
    const runtime = createSandboxedPreloadRuntime({electron:allElectron,
      process:sourceProcess, Buffer:buffer, EventEmitter:Emitter,
      loadBuiltin: name => {
        loaded.push(name);
        if (name === 'events') return events;
        if (name === 'timers') return timers;
        if (name === 'url') return {URL:'url-constructor'};
        throw Error('Privileged module loader reached');
      }});
    runtime.removePrivilegedGlobals();
  `, context);
  return source => vm.runInContext(source, context);
}

test('sandbox preload uses the official limited module set without package resolution', () => {
  const evaluate = fixture();
  assert.equal(evaluate("runtime.require('electron') === runtime.require('electron/common')"), true);
  assert.equal(evaluate("runtime.require('electron') === runtime.require('electron/renderer')"), true);
  for (const name of ['events', 'timers', 'url']) {
    assert.equal(evaluate(`runtime.require('${name}') === runtime.require('node:${name}')`), true);
  }
  for (const name of ['fs', 'node:fs', 'path', 'buffer', 'child_process', 'module',
    'process', 'node:electron', './other.js', '/absolute.js', 'addon.node', '#imports']) {
    assert.throws(() => evaluate(`runtime.require(${JSON.stringify(name)})`), {code:'MODULE_NOT_FOUND'});
  }
  assert.equal(evaluate("runtime.require.resolve"), undefined);
  assert.equal(evaluate("JSON.stringify(Object.keys(runtime.electron).sort())"),
    JSON.stringify(['contextBridge', 'crashReporter', 'ipcRenderer', 'nativeImage', 'webFrame', 'webUtils']));
});

test('lexical preload APIs work after raw globals and native transports are revoked', () => {
  const evaluate = fixture();
  evaluate(`runtime.run('/application/preload.js',
    "const {ipcRenderer,contextBridge}=require('electron');" +
    "contextBridge.exposeInMainWorld('publicApi',{value:Buffer.from('ok').value});" +
    "ipcRenderer.send('application:ready',process.platform);" +
    "global.leaks=[global.require,global.process,global.electron,global.xenonIpcRenderer," +
    "global.__xenonElectronRequire,global.__xenonElectronIpc,global.__xenonPaths," +
    "typeof __filename,typeof __dirname,module.require];" +
    "global.immediate=setImmediate(()=>global.called=true);");`);
  assert.equal(evaluate('publicApi.value'), 'ok');
  assert.equal(evaluate('JSON.stringify(sent)'), '[["application:ready","win32"]]');
  assert.equal(evaluate('leaks.every((value,index)=>index===7||index===8 ? value==="undefined" : value===undefined)'), true);
  assert.equal(evaluate('immediate'), 10);
  assert.equal(evaluate('called'), true);
});

test('sandbox process has isolated metadata and no general Node entry points', () => {
  const evaluate = fixture();
  for (const name of ['_linkedBinding', 'binding', 'cwd', 'nextTick', 'mainModule', 'exit']) {
    assert.equal(evaluate(`runtime.process[${JSON.stringify(name)}]`), undefined);
  }
  evaluate("runtime.process.env.EXAMPLE='changed';runtime.process.argv.push('extra');runtime.process.versions.electron='changed'");
  assert.equal(evaluate('sourceProcess.env.EXAMPLE'), 'value');
  assert.equal(evaluate('sourceProcess.argv.length'), 1);
  assert.equal(evaluate('sourceProcess.versions.electron'), '39');
  assert.equal(evaluate('runtime.process.getSystemVersion()'), 'native-version');
  assert.throws(() => evaluate('runtime.process.getProcessMemoryInfo()'), {code:'ERR_NOT_SUPPORTED'});
  assert.throws(() => evaluate('runtime.electron.webUtils.getPathForFile({})'), {code:'ERR_NOT_SUPPORTED'});
});

test('missing OS sandbox and unrevocable native globals fail closed', () => {
  const evaluate = fixture();
  assert.throws(() => evaluate(`createSandboxedPreloadRuntime({electron:allElectron,
    process:{sandboxed:false},Buffer:buffer,EventEmitter:Emitter,loadBuiltin(){}})`),
    {code:'ERR_SANDBOX_INIT_FAILED'});
  evaluate("Object.defineProperty(globalThis,'xenonIpcRenderer',{value:{},configurable:false})");
  assert.throws(() => evaluate('runtime.removePrivilegedGlobals()'), {code:'ERR_SANDBOX_INIT_FAILED'});
  assert.throws(() => evaluate("runtime.run('/unregistered.js')"), {code:'ERR_SANDBOX_PRELOAD_SOURCE'});
});

test('app.enableSandbox is synchronous, before ready and requires native acknowledgement', () => {
  const factory = vm.runInNewContext(fs.readFileSync(path.join(__dirname,'main/sandbox.js'),'utf8') +
    '\ncreateElectronSandboxApi');
  let ready = false, calls = 0;
  const api = factory({isReady:()=>ready,callHost:()=>{++calls;return true;}});
  assert.equal(api.enableSandbox(), undefined);
  assert.equal(calls, 1);
  ready = true;
  assert.throws(()=>api.enableSandbox(), {code:'ERR_INVALID_STATE'});
  assert.equal(calls, 1);
  assert.throws(()=>factory({isReady:()=>false}).enableSandbox(), {code:'ERR_NOT_SUPPORTED'});
  assert.throws(()=>factory({isReady:()=>false,callHost:()=>undefined}).enableSandbox(),
    {code:'ERR_IPC_INVALID_RESPONSE'});
  const denied = Object.assign(new Error('host has no sandbox'), {code:'ERR_NOT_SUPPORTED'});
  assert.throws(()=>factory({isReady:()=>false,callHost:()=>{throw denied;}}).enableSandbox(), error=>error===denied);
});

test('window defaults and explicit Node integration change only after native sandbox acknowledgement', () => {
  const factory = vm.runInNewContext(fs.readFileSync(path.join(__dirname,'main/sandbox.js'),'utf8') +
    '\ncreateElectronSandboxApi');
  const preferences = {sandbox:false,nodeIntegration:true,preload:'/app/preload.js'};
  let accepted = false, calls = 0;
  const api = factory({isReady:()=>false,callHost:request=>{
    ++calls; assert.equal(request.action, 'enable'); return accepted;
  }});
  assert.equal(api.normalizePreferences({}).sandbox, true);
  assert.equal(api.normalizePreferences({nodeIntegration:true}).sandbox, false);
  assert.equal(api.normalizePreferences({nodeIntegrationInWorker:true}).sandbox, false);
  assert.equal(api.normalizePreferences(preferences).nodeIntegration, true);
  assert.equal(calls, 0);
  assert.throws(()=>api.enableSandbox(), {code:'ERR_IPC_INVALID_RESPONSE'});
  assert.equal(api.normalizePreferences(preferences).sandbox, false);
  accepted = true;
  api.enableSandbox();
  const forced = api.normalizePreferences(preferences);
  assert.equal(forced.sandbox, true);
  assert.equal(forced.nodeIntegration, false);
  assert.equal(forced.preload, preferences.preload);
  assert.equal(preferences.sandbox, false);
  assert.equal(api.normalizePreferences({contextIsolation:false}).contextIsolation, false);
  assert.throws(()=>api.normalizePreferences({sandbox:'true'}), {code:'ERR_INVALID_ARG_TYPE'});
});

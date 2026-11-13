// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createSandboxedPreloadRuntime({electron, process: sourceProcess,
  Buffer, EventEmitter, loadBuiltin, globalObject = globalThis}) {
  // Revoke the unrestricted runtime even when OS sandbox verification fails.
  // An initialization exception must never leave the full Node broker exposed.
  removePrivilegedGlobals();
  if (sourceProcess.sandboxed !== true) {
    throw Object.assign(new Error('The renderer OS sandbox is not active'),
      {code: 'ERR_SANDBOX_INIT_FAILED'});
  }
  const unsupported = name => {
    throw Object.assign(new Error(`${name} is not supported in this sandboxed preload`),
      {code: 'ERR_NOT_SUPPORTED'});
  };
  const sandboxElectron = {};
  // Electron's sandboxed_renderer/api/module-list, not the larger ordinary
  // renderer/common lists: clipboard, shell, app and remote are not included.
  for (const name of ['contextBridge', 'crashReporter', 'ipcRenderer',
    'nativeImage', 'webFrame', 'webUtils']) {
    if (electron[name] !== undefined) sandboxElectron[name] = electron[name];
  }
  if (!sandboxElectron.webUtils) {
    sandboxElectron.webUtils = {
      getPathForFile() { return unsupported('webUtils.getPathForFile'); },
    };
  }

  const process = new EventEmitter();
  for (const name of ['arch', 'platform', 'version', 'execPath', 'pid', 'type',
    'sandboxed', 'contextId', 'contextIsolated', 'isMainFrame', 'mas', 'windowsStore']) {
    if (sourceProcess[name] !== undefined) process[name] = sourceProcess[name];
  }
  process.argv = Array.from(sourceProcess.argv || []);
  process.env = {...sourceProcess.env};
  process.versions = {...sourceProcess.versions};
  for (const name of ['crash', 'hang', 'getCreationTime', 'getHeapStatistics',
    'getBlinkMemoryInfo', 'getSystemMemoryInfo', 'getSystemVersion', 'getCPUUsage',
    'getProcessMemoryInfo', 'uptime']) {
    process[name] = typeof sourceProcess[name] === 'function'
      ? sourceProcess[name].bind(sourceProcess)
      : () => unsupported(`process.${name}`);
  }
  Object.defineProperty(process, 'noDeprecation', {
    enumerable: true,
    get: () => sourceProcess.noDeprecation,
    set: value => { sourceProcess.noDeprecation = value; },
  });

  const modules = new Map([
    ['electron', sandboxElectron], ['electron/common', sandboxElectron],
    ['electron/renderer', sandboxElectron],
  ]);
  function preloadRequire(request) {
    if (modules.has(request)) return modules.get(request);
    // No filesystem/package resolver is reachable from this function. A
    // preload must be bundled, exactly as with Electron's sandboxed runtime.
    if (typeof request === 'string' &&
        /^(?:node:)?(?:events|timers|url)$/.test(request)) {
      const name = request.replace(/^node:/, '');
      const value = loadBuiltin(name);
      modules.set(name, value);
      modules.set('node:' + name, value);
      return value;
    }
    throw Object.assign(new Error(`module not found: ${String(request)}`),
      {code: 'MODULE_NOT_FOUND'});
  }

  function removePrivilegedGlobals() {
    // The bootstrap's complete runtime stays in its private closure. Merely
    // passing a limited lexical require is insufficient while global.require
    // or the raw native transport remains reachable by the preload.
    for (const name of ['require', 'process', 'Buffer', 'global', '__filename',
      '__dirname', 'xenonIpcRenderer', '__xenonElectronRequire',
      '__xenonElectronIpc', '__xenonPaths', '__xenonPageHandler__',
      '__xenonPageCallbackRouter__', '__xenonPageHandlerBound__']) {
      if (!Reflect.deleteProperty(globalObject, name)) {
        throw Object.assign(new Error(`Cannot revoke preload global ${name}`),
          {code: 'ERR_SANDBOX_INIT_FAILED'});
      }
    }
    if (globalObject.electron === electron &&
        !Reflect.deleteProperty(globalObject, 'electron')) {
      throw Object.assign(new Error('Cannot revoke unrestricted Electron alias'),
        {code: 'ERR_SANDBOX_INIT_FAILED'});
    }
  }

  return {
    require: preloadRequire,
    process,
    electron: sandboxElectron,
    removePrivilegedGlobals,
    run(filePath, source) {
      if (typeof source !== 'string') {
        throw Object.assign(new Error('Browser did not provide the registered preload source'),
          {code: 'ERR_SANDBOX_PRELOAD_SOURCE'});
      }
      const timers = preloadRequire('timers');
      const safeName = String(filePath).replace(/[\r\n]/g, '').replace(/\\/g, '/');
      const compile = new Function('require', 'process', 'exports', 'module',
        'Buffer', 'global', 'setImmediate', 'clearImmediate',
        source.replace(/^\uFEFF/, '').replace(/^#![^\r\n]*/, '') +
          '\n//# sourceURL=' + safeName);
      const module = {exports: {}};
      compile(preloadRequire, process, module.exports, module, Buffer, globalObject,
        timers.setImmediate, timers.clearImmediate);
      return module.exports;
    },
  };
}

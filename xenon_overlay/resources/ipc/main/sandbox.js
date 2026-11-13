// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createElectronSandboxApi({isReady, callHost}) {
  let forced = false;
  function requireHost() {
    if (typeof callHost !== 'function') {
      throw Object.assign(new Error('Native Electron sandbox policy is unavailable'),
        {code: 'ERR_NOT_SUPPORTED'});
    }
  }
  return {
    enableSandbox() {
      if (isReady()) {
        throw Object.assign(new Error('app.enableSandbox() can only be called before app is ready'),
          {code: 'ERR_INVALID_STATE'});
      }
      // The Browser authenticates the owner and must acknowledge that the
      // real launch configuration supports sandboxed renderer processes.
      // A local flag, or the main utility process's argv, is not evidence.
      requireHost();
      if (callHost({action: 'enable'}) !== true) {
        throw Object.assign(new Error('Native host did not enable the Electron sandbox policy'),
          {code: 'ERR_IPC_INVALID_RESPONSE'});
      }
      forced = true;
    },
    normalizePreferences(preferences) {
      const result = {...preferences};
      for (const key of ['sandbox', 'contextIsolation', 'nodeIntegration',
        'nodeIntegrationInWorker', 'nodeIntegrationInSubFrames', 'webviewTag']) {
        if (result[key] === undefined) delete result[key];
        else if (typeof result[key] !== 'boolean') {
          throw Object.assign(new TypeError(`${key} must be a boolean`),
            {code: 'ERR_INVALID_ARG_TYPE'});
        }
      }
      // This is only the public main-process preference snapshot. Browser
      // independently applies the same defaults and its authenticated owner
      // policy at every window/guest/document boundary.
      const sandbox = forced || (result.sandbox ??
        !(result.nodeIntegration || result.nodeIntegrationInWorker));
      result.sandbox = sandbox;
      result.contextIsolation ??= true;
      result.nodeIntegration = !sandbox && result.nodeIntegration === true;
      result.nodeIntegrationInWorker = !sandbox && result.nodeIntegrationInWorker === true;
      return result;
    },
  };
}

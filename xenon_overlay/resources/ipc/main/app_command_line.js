// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createAppCommandLine(callHost) {
  const requireString = (value, name) => {
    if (typeof value !== 'string')
      throw Object.assign(new TypeError(`${name} must be a string`), {code:'ERR_INVALID_ARG_TYPE'});
    if (value.includes('\0'))
      throw Object.assign(new TypeError(`${name} must not contain NUL`), {code:'ERR_INVALID_ARG_VALUE'});
    return value;
  };
  const call = (method, details) => {
    try { return callHost({method, ...details}); }
    catch (error) {
      const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
      if (code && !error.code) error.code = code[1];
      throw error;
    }
  };
  return {
    appendSwitch(name, value = '') {
      call('appendSwitch', {name:requireString(name, 'switch'),value:requireString(value, 'value')});
    },
    appendArgument(value) { call('appendArgument', {value:requireString(value, 'argument')}); },
    removeSwitch(name) { call('removeSwitch', {name:requireString(name, 'switch')}); },
    hasSwitch(name) {
      const result = call('hasSwitch', {name:requireString(name, 'switch')});
      if (typeof result !== 'boolean')
        throw Object.assign(new Error('Invalid native command line response'), {code:'ERR_IPC_INVALID_RESPONSE'});
      return result;
    },
    getSwitchValue(name) {
      const result = call('getSwitchValue', {name:requireString(name, 'switch')});
      if (typeof result !== 'string')
        throw Object.assign(new Error('Invalid native command line response'), {code:'ERR_IPC_INVALID_RESPONSE'});
      return result;
    },
  };
}

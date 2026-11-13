// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createCrashReporter({callLocal, callHost, main = false, app}) {
  function fail(code, message, Type = Error) {
    const error = new Type(message);
    error.code = code;
    throw error;
  }
  function call(callback, operation, details = {}) {
    if (typeof callback !== 'function') {
      fail('ERR_NOT_SUPPORTED', 'The native crashReporter binding is unavailable');
    }
    try {
      return callback(operation, details);
    } catch (error) {
      if (error && !error.code && typeof error.message === 'string') {
        const code = /^([A-Z][A-Z_]+):/.exec(error.message);
        if (code) error.code = code[1];
      }
      throw error;
    }
  }
  function string(value, name) {
    if (typeof value !== 'string') {
      fail('ERR_INVALID_ARG_TYPE', `${name} must be a string`, TypeError);
    }
    if (value.includes('\0')) {
      fail('ERR_INVALID_ARG_VALUE', `${name} must not contain NUL characters`);
    }
    return value;
  }
  function record(value, name) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
      fail('ERR_INVALID_ARG_TYPE', `${name} must be an object`, TypeError);
    }
    const copy = Object.create(null);
    for (const key of Object.keys(value)) copy[key] = string(value[key], `${name}.${key}`);
    return copy;
  }
  const api = {
    addExtraParameter(key, value) {
      call(callLocal, 'addExtraParameter', {
        key: string(key, 'key'), value: string(value, 'value'),
      });
    },
    removeExtraParameter(key) {
      call(callLocal, 'removeExtraParameter', {key: string(key, 'key')});
    },
    getParameters() {
      return call(callLocal, 'getParameters');
    },
  };
  if (!main) return api;

  api.start = (options = {}) => {
    if (!options || typeof options !== 'object' || Array.isArray(options)) {
      fail('ERR_INVALID_ARG_TYPE', 'crashReporter options must be an object', TypeError);
    }
    const {
      companyName, productName = app.getName(), extra = {}, globalExtra = {},
      submitURL = '', uploadToServer = true, ignoreSystemCrashHandler = false,
      rateLimit = false, compress = true,
    } = options;
    string(submitURL, 'submitURL');
    for (const [name, value] of Object.entries({
      uploadToServer, ignoreSystemCrashHandler, rateLimit, compress,
    })) {
      if (typeof value !== 'boolean') {
        fail('ERR_INVALID_ARG_TYPE', `${name} must be a boolean`, TypeError);
      }
    }
    const globals = record(globalExtra, 'globalExtra');
    if (Object.keys(globals).length) {
      fail('ERR_NOT_SUPPORTED', 'crashReporter.globalExtra cannot be propagated to every application process');
    }
    const details = {
      productName: string(productName, 'productName'),
      appVersion: string(app.getVersion(), 'app version'),
      extra: record(extra, 'extra'), globalExtra: globals,
    };
    if (companyName !== undefined) details.companyName = string(companyName, 'companyName');
    // The Chromium host already owns Crashpad. Application options do not
    // restart it or change upload consent, URL, compression, rate limiting or
    // system forwarding. In particular uploadToServer:false does NOT disable
    // the host's uploads. See docs/Electron崩溃报告兼容.md.
    call(callLocal, 'start', details);
  };
  api.getUploadToServer = () => call(callHost, 'crashReporter.getUploadToServer');
  api.setUploadToServer = uploadToServer => {
    if (typeof uploadToServer !== 'boolean') {
      fail('ERR_INVALID_ARG_TYPE', 'uploadToServer must be a boolean', TypeError);
    }
    // Explicit host-managed compatibility no-op. Never cache this value as if
    // it had changed the real policy; the getter always reads the host state.
  };
  api.getUploadedReports = () => {
    const reports = call(callHost, 'crashReporter.getUploadedReports');
    return reports.map(report => ({id: report.id, date: new Date(report.date)}));
  };
  api.getLastCrashReport = () => api.getUploadedReports()[0] || null;
  return api;
}

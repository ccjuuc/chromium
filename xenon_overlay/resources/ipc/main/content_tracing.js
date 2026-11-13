// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createContentTracing({callHost, resolvePath}) {
  const invalid = message => Object.assign(new TypeError(message), {code:'ERR_INVALID_ARG_TYPE'});
  const call = (method, details = {}) => Promise.resolve().then(() =>
    callHost({method, ...details}));
  return {
    getCategories() { return call('getCategories'); },
    getTraceBufferUsage() { return call('getTraceBufferUsage'); },
    startRecording(options) {
      if (!options || typeof options !== 'object' || Array.isArray(options))
        throw invalid('contentTracing.startRecording requires an options object');
      // Snapshot before asynchronous IPC so later application mutations cannot
      // change the requested native recording configuration.
      return call('startRecording', {options: structuredClone(options)}).then(() => undefined);
    },
    stopRecording(resultFilePath = '') {
      if (typeof resultFilePath !== 'string' || resultFilePath.includes('\0'))
        throw invalid('contentTracing.stopRecording requires a file path string');
      return call('stopRecording', {path: resultFilePath ? resolvePath(resultFilePath) : ''});
    },
  };
}

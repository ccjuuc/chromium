// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(() => {
  'use strict';

  // @include "main/events.js"

  // @include "main/async_context.js"

  // @include "main/platform.js"

  // @include "main/app.js"

  // @include "main/windows.js"

  // @include "main/buffer.js"

  // @include "main/fs.js"


  // @include "main/builtins.js"

  // @include "main/child_process.js"

  function assert(condition, message) {
    if (!condition) throw new Error(message || 'Assertion failed');
  }
  assert.ok = assert;
  assert.strictEqual = (a, b, m) => { if (a !== b) throw new Error(m || `Expected ${a} === ${b}`); };
  assert.deepStrictEqual = (a, b, m) => assert.strictEqual(JSON.stringify(a), JSON.stringify(b), m);
  assert.equal = assert.strictEqual;
  assert.notEqual = (a, b, m) => { if (a === b) throw new Error(m || `Expected ${a} !== ${b}`); };

  // @include "common/zlib.js"
  const zlibModule = createZlibModule((operation, data, options, asynchronous) =>
      __xenonZlibCall(operation, data, options, asynchronous));
  const constantsModule = Object.assign({}, fsConstants, zlibModule.constants);
  ipcMain.on('__xenon:zlib', (event, request) => {
    event.returnValue = Buffer.from(__xenonZlibCall(request.operation,
        Buffer.from(request.dataBase64, 'base64'), request.options || {}, false))
        .toString('base64');
  });
  ipcMain.handle('__xenon:zlib', async (_event, request) => {
    const result = await __xenonZlibCall(request.operation,
        Buffer.from(request.dataBase64, 'base64'), request.options || {}, true);
    return Buffer.from(result).toString('base64');
  });
  // @include "main/net.js"
  // @include "main/http.js"
  // @include "main/http2.js"
  const ttyModule = { isatty: () => false };
  // @include "main/tls.js"

  // Non-terminal readline consumes real stream data and preserves UTF-8 and
  // CRLF boundaries across chunks. Interactive terminal editing is separate.
  // @include "common/readline.js"
  // @include "main/install.js"
})();

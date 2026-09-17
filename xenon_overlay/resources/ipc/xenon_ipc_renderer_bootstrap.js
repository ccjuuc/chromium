// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Renderer-side compatibility layer for the generic Browser-process Electron
// container. It provides standard Electron and Node.js APIs (ipcRenderer, path,
// os, fs, events, buffer, util, process, and Mojo Node-API native addon loading).
(() => {
  'use strict';

  // @include "renderer/runtime_config.js"

  // @include "renderer/events.js"

  // @include "renderer/runtime.js"

  // @include "renderer/platform.js"

  // @include "renderer/buffer.js"

  // @include "renderer/fs.js"

  // @include "renderer/native_bridge.js"

  // @include "renderer/native_modules.js"

  // @include "renderer/net.js"

  // @include "renderer/builtins.js"

  // --- 9. Standard globalThis.require ---
  const hostedCjsCache = Object.create(null);
  const moduleResolutionCache = new Map();
  // @include "common/zlib.js"
  let canonicalModuleRoots;
  const builtinModuleCache = new Map();
  const builtinModuleNames = new Set([
    'assert', 'async_hooks', 'buffer', 'child_process', 'constants', 'crypto',
    'dns', 'events', 'fs', 'fs/promises', 'http', 'http2', 'https', 'net', 'os',
    'path', 'path/posix', 'path/win32', 'process', 'querystring', 'readline', 'stream',
    'string_decoder', 'timers', 'tls', 'tty', 'url', 'util', 'zlib',
  ]);
  // @include "renderer/tls.js"

  // Non-terminal readline consumes real stream data and preserves UTF-8 and
  // CRLF boundaries across chunks. Interactive terminal editing is separate.
  // @include "common/readline.js"
  // @include "renderer/child_process.js"
  // @include "renderer/http2.js"
  // @include "renderer/modules.js"

  // @include "renderer/page.js"
})();

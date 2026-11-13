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
  // @include "common/events_helpers.js"

  // @include "common/web_streams.js"
  let getWebStreamState;
  const webStreamsModule = createWebStreamsModule(globalThis, () => {
    const module = {exports: {}};
    const exports = module.exports;
    // @include "third_party/web_streams_polyfill/dist/ponyfill.js"
    return module.exports;
  }, reader => { getWebStreamState = reader; });

  // @include "common/clipboard.js"
  // @include "common/native_image.js"
  // @include "common/crash_reporter.js"
  // @include "renderer/runtime.js"

  // @include "renderer/platform.js"

  // @include "common/buffer_numeric.js"
  // @include "common/buffer.js"
  // @include "common/promisify.js"
  // @include "renderer/buffer.js"

  // @include "third_party/readable_stream/bundle.js"
  // @include "common/stream_web_adapters.js"
  // @include "common/node_streams.js"
  const nodeStreams = createNodeStreams({Buffer, EventEmitter,
    modules: createReadableStreamModules(), webStreams: webStreamsModule, getWebStreamState});
  const streamModule = nodeStreams.stream;
  const {Stream, Readable, Writable, Duplex, Transform, PassThrough} = streamModule;
  const stringDecoderModule = nodeStreams.stringDecoder;
  // @include "common/file_streams.js"

  // @include "common/deep_equal.js"
  // Capture local V8 predicates before Node-disabled pages drop the transport.
  const nativeUtilTypes = {
    isPromise: transport.isPromise?.bind(transport),
    isNativeError: transport.isNativeError?.bind(transport),
  };
  for (const name of Object.keys(nativeUtilTypes)) {
    if (typeof nativeUtilTypes[name] === 'function')
      utilModule.types[name] = nativeUtilTypes[name];
  }
  utilModule.isDeepStrictEqual = createIsDeepStrictEqual(nativeUtilTypes);

  // @include "common/util_text.js"
  utilModule.stripVTControlCharacters = stripVTControlCharacters;
  // @include "common/util_debuglog.js"
  // @include "common/console.js"

  // @include "common/fs_stats.js"
  // @include "common/fs_mkdtemp.js"
  // @include "common/native_fs.js"
  // @include "renderer/fs.js"

  // @include "renderer/native_bridge.js"
  const rendererConsole = globalThis.console;
  installNodeDebuglog(utilModule, process, EventEmitter, Buffer,
      text => rendererConsole.error(text.endsWith('\n') ? text.slice(0, -1) : text));
  const consoleModule = installNodeConsole({util: utilModule, process, EventEmitter, Buffer,
    writeHost: text => rendererConsole.log(text.endsWith('\n') ? text.slice(0, -1) : text),
    now: () => globalThis.performance.now(),
  });

  // @include "renderer/native_modules.js"

  // @include "common/net.js"
  // @include "renderer/net.js"

  // @include "common/crypto_random.js"
  // @include "renderer/builtins.js"

  // @include "common/x509_certificate.js"
  const nativeX509CertificateCall = transport.x509CertificateCall?.bind(transport);
  cryptoModule.X509Certificate = createX509CertificateClass({Buffer,
    call: (...args) => {
      if (!nativeX509CertificateCall) throw Object.assign(
        new Error('Native X.509 parsing is unavailable'), {code:'ERR_NOT_SUPPORTED'});
      return nativeX509CertificateCall(...args);
    },
  });

  // --- 9. Standard globalThis.require ---
  const hostedCjsCache = Object.create(null);
  const moduleResolutionCache = new Map();
  // @include "common/zlib.js"
  let canonicalModuleRoots;
  const builtinModuleCache = new Map();
  const builtinModuleNames = new Set([
    'assert', 'async_hooks', 'buffer', 'child_process', 'console', 'constants', 'crypto',
    'dns', 'events', 'fs', 'fs/promises', 'http', 'http2', 'https', 'module', 'net',
    'original-fs', 'original-fs/promises', 'os',
    'path', 'path/posix', 'path/win32', 'process', 'querystring', 'readline', 'stream',
    'stream/promises', 'stream/web', 'string_decoder', 'timers', 'timers/promises', 'tls', 'tty', 'url', 'util', 'zlib',
  ]);
  // @include "renderer/tls.js"

  // Non-terminal readline consumes real stream data and preserves UTF-8 and
  // CRLF boundaries across chunks. Interactive terminal editing is separate.
  // @include "common/readline.js"
  // @include "common/child_process.js"
  // @include "renderer/child_process.js"
  // @include "renderer/http2.js"
  // @include "common/http_server.js"
  // @include "common/http_upgrade.js"
  // @include "common/module.js"
  // @include "common/package_maps.js"
  // @include "common/timers_promises.js"
  // @include "common/timers.js"
  let rendererNodeTimers;
  // @include "renderer/modules.js"

  // @include "renderer/sandbox.js"

  // @include "renderer/page.js"
})();

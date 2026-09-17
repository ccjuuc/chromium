// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const nativeHttp2 = require('node:http2');
const nativeChildProcess = require('node:child_process');
const test = require('node:test');
const {promisify} = require('node:util');
const vm = require('node:vm');

function loadModules(side) {
  const source = ['events', 'child_process', 'http2']
      .map(name => readBootstrapPart(`${side}/${name}.js`)).join('\n');
  const context = vm.createContext({queueMicrotask});
  vm.runInContext(source +
      '\nglobalThis.modules = {childProcessModule, http2Module};', context);
  return context.modules;
}

for (const side of ['main', 'renderer']) {
  test(`${side}: child_process imports expose the Node API without fabricating a process`, async () => {
    const {childProcessModule: cp} = loadModules(side);
    assert.deepEqual(Object.keys(cp).sort(), Object.keys(nativeChildProcess).sort());
    let callbacks = 0;
    for (const method of ['spawn', 'spawnSync', 'exec', 'execSync', 'execFile',
                          'execFileSync', 'fork', '_forkChild']) {
      assert.equal(typeof cp[method], 'function');
      assert.throws(() => cp[method]('unavailable.exe'), {code: 'ERR_NOT_SUPPORTED'});
      assert.throws(() => cp[method]('unavailable.exe', () => ++callbacks),
                    {code: 'ERR_NOT_SUPPORTED'});
    }
    assert.throws(() => new cp.ChildProcess(), {code: 'ERR_NOT_SUPPORTED'});
    await Promise.resolve();
    assert.equal(callbacks, 0);
    // Libraries promisify exec while importing; actual execution still rejects.
    await assert.rejects(promisify(cp.exec)('unavailable.exe'), {code: 'ERR_NOT_SUPPORTED'});
  });

  test(`${side}: HTTP/2 constants and independent export names match Node`, () => {
    const {http2Module: http2} = loadModules(side);
    assert.deepEqual(Object.keys(http2).sort(), Object.keys(nativeHttp2).sort());
    for (const name of ['HTTP2_HEADER_SCHEME', 'HTTP2_HEADER_METHOD',
                       'HTTP2_HEADER_PATH', 'HTTP2_HEADER_STATUS', 'HTTP2_HEADER_AUTHORITY']) {
      assert.equal(http2.constants[name], nativeHttp2.constants[name]);
    }
    assert.equal(typeof http2.sensitiveHeaders, 'symbol');
    assert.equal(http2.sensitiveHeaders.description, nativeHttp2.sensitiveHeaders.description);
    const settings = http2.getDefaultSettings();
    assert.equal(Object.getPrototypeOf(settings), null);
    assert.deepEqual(JSON.parse(JSON.stringify(settings)),
                     JSON.parse(JSON.stringify(nativeHttp2.getDefaultSettings())));
    settings.enablePush = false;
    assert.equal(http2.getDefaultSettings().enablePush, true);
  });

  test(`${side}: HTTP/2 transport and unimplemented codecs fail only when called`, () => {
    const {http2Module: http2} = loadModules(side);
    let callbacks = 0;
    for (const method of ['connect', 'createServer', 'createSecureServer',
        'performServerHandshake', 'getPackedSettings', 'getUnpackedSettings']) {
      assert.equal(typeof http2[method], 'function');
      assert.throws(() => http2[method]({}, () => ++callbacks), {code: 'ERR_NOT_SUPPORTED'});
    }
    for (const name of ['Http2ServerRequest', 'Http2ServerResponse']) {
      assert.throws(() => new http2[name](), {code: 'ERR_NOT_SUPPORTED'});
    }
    assert.equal(callbacks, 0);
  });
}

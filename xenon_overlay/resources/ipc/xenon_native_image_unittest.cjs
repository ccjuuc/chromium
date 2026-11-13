// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const zlib = require('node:zlib');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function chunk(type, data) {
  const result = Buffer.alloc(data.length + 12);
  result.writeUInt32BE(data.length);
  result.write(type, 4);
  data.copy(result, 8);
  result.writeUInt32BE(zlib.crc32(result.subarray(4, -4)), result.length - 4);
  return result;
}
const header = Buffer.alloc(13);
header.writeUInt32BE(2); header.writeUInt32BE(1, 4); header[8] = 8; header[9] = 6;
const png = Buffer.concat([Buffer.from('89504e470d0a1a0a', 'hex'), chunk('IHDR', header),
  chunk('IDAT', zlib.deflateSync(Buffer.from([0, 255, 0, 0, 255, 0, 255, 0, 128]))),
  chunk('IEND', Buffer.alloc(0))]);
const pngBase64 = png.toString('base64');
const descriptor = {pngBase64, width: 2, height: 1};
const empty = {pngBase64: '', width: 0, height: 0};

// Native codec/pixel behavior is tested in XenonNativeImageBridgeTest. This
// mock isolates the JS image identity, argument and transport contracts.
function fixture(callOverride, readOverride) {
  const calls = [];
  const reads = [];
  const call = (operation, details) => {
    calls.push({operation, ...JSON.parse(JSON.stringify(details))});
    if (callOverride) return callOverride(operation, details);
    if (operation === 'nativeImage.decode') return {...(details.dataBase64 === pngBase64 ||
      details.dataURL === 'data:image/png;base64,' + pngBase64 ? descriptor : empty)};
    if (operation === 'nativeImage.toBitmap') return Buffer.from([1, 2, 3, 4, 5, 6, 7, 8]).toString('base64');
    if (operation === 'nativeImage.toJPEG') return Buffer.from([255, 216, 255, 217]).toString('base64');
    assert.fail(operation);
  };
  const read = filename => { reads.push(filename); return readOverride ? readOverride(filename) : png; };
  const context = vm.createContext({Buffer, call, read});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'common/native_image.js'), 'utf8') +
      ';globalThis.api = createNativeImageApi({callSync:call,readFileSync:read});', context);
  return {api: context.api, calls, reads, call, read};
}

test('empty NativeImage has Electron dimensions, scale factors and encodings', () => {
  const {api, calls} = fixture();
  const image = api.createEmpty();
  assert.equal(image.isEmpty(), true);
  assert.deepEqual({...image.getSize()}, {width: 0, height: 0});
  assert.equal(image.getAspectRatio(), 1);
  assert.deepEqual(Array.from(image.getScaleFactors()), []);
  assert.equal(image.toPNG().length, 0);
  assert.equal(image.toJPEG(80).length, 0);
  assert.equal(image.toBitmap().length, 0);
  assert.equal(image.toDataURL(), 'data:image/png;base64,');
  assert.equal(calls.length, 0);
});

test('decoded images preserve bytes and return independent buffers and sizes', () => {
  const {api} = fixture();
  const image = api.createFromBuffer(png);
  assert.equal(image.isEmpty(), false);
  assert.equal(image.getAspectRatio(), 2);
  assert.deepEqual({...image.getSize()}, {width: 2, height: 1});
  assert.deepEqual(Array.from(image.getScaleFactors()), [1]);
  const bytes = image.toPNG(); bytes.fill(0);
  image.getSize().width = 999;
  assert.deepEqual(image.toPNG(), png);
  assert.equal(image.getSize().width, 2);
  assert.equal(image.toDataURL({scaleFactor: 1}), 'data:image/png;base64,' + pngBase64);
  assert.equal(api.createFromDataURL(image.toDataURL()).isEmpty(), false);
  assert.equal(Object.prototype.toString.call(image), '[object NativeImage]');
});

test('path creation reads through filesystem including ASAR and retains valid menu paths', () => {
  const {api, reads} = fixture();
  const filename = '/release/resources/app.asar/icons/app.png';
  const image = api.createFromPath(filename);
  assert.deepEqual(reads, [filename]);
  assert.equal(image.path, filename);
  assert.equal(Object.getOwnPropertyDescriptor(image, 'path').writable, false);
  assert.deepEqual(image.toPNG(), png);
  const bad = fixture(undefined, () => Buffer.from('invalid')).api.createFromPath('/invalid.png');
  assert.equal(bad.isEmpty(), true);
  assert.equal(bad.path, undefined);
  for (const code of ['ENOENT', 'EACCES', 'EISDIR']) {
    const missing = fixture(undefined, () => { throw Object.assign(new Error(code), {code}); });
    assert.equal(missing.api.createFromPath('/missing.png').isEmpty(), true);
  }
});

test('native conversions forward quality and return native bytes without hidden handles', () => {
  const {api, calls} = fixture();
  const image = api.createFromBuffer(png);
  assert.deepEqual(image.toJPEG(73), Buffer.from([255, 216, 255, 217]));
  assert.deepEqual(image.toBitmap(), Buffer.from([1, 2, 3, 4, 5, 6, 7, 8]));
  assert.deepEqual(calls.slice(1), [
    {operation: 'nativeImage.toJPEG', pngBase64, quality: 73},
    {operation: 'nativeImage.toBitmap', pngBase64},
  ]);
  assert.equal(image.pngBase64, undefined);
  assert.throws(() => image.getNativeHandle(), {code: 'ERR_NOT_SUPPORTED'});
});

test('invalid data becomes empty while unsupported operations and bad types fail explicitly', async () => {
  const {api} = fixture();
  assert.equal(api.createFromBuffer(Buffer.from('broken')).isEmpty(), true);
  assert.equal(api.createFromDataURL('bad data').isEmpty(), true);
  assert.equal(api.createFromBuffer(Buffer.alloc(0)).isEmpty(), true);
  const image = api.createFromBuffer(png);
  for (const value of [undefined, null, '', {}, new Uint8Array([1])])
    assert.throws(() => api.createFromBuffer(value), {code: 'ERR_INVALID_ARG_TYPE'});
  for (const value of [undefined, null, 42, {}]) {
    assert.throws(() => api.createFromDataURL(value), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => api.createFromPath(value), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  for (const operation of [() => image.resize({width: 1}), () => image.crop({}),
    () => image.addRepresentation({}), () => image.toPNG({scaleFactor: 2}),
    () => image.getSize(2), () => image.toBitmap({colorSpace: 'srgb'}),
    () => api.createFromBuffer(png, {width: 2, height: 1}),
    () => api.createFromBitmap(Buffer.alloc(4), {width: 1, height: 1})])
    assert.throws(operation, {code: 'ERR_NOT_SUPPORTED'});
  for (const value of [-1, 101, 1.5, NaN, undefined, '80'])
    assert.throws(() => image.toJPEG(value), {code: 'ERR_OUT_OF_RANGE'});
  await assert.rejects(api.createThumbnailFromPath('/image.png', {width: 1, height: 1}),
      {code: 'ERR_NOT_SUPPORTED'});
});

test('native and filesystem failures are preserved and malformed descriptions are rejected', () => {
  const failure = Object.assign(new Error('unavailable'), {code: 'ERR_NOT_SUPPORTED'});
  const broken = fixture(() => { throw failure; });
  assert.throws(() => broken.api.createFromBuffer(png), error => error === failure);
  const missing = fixture(undefined, () => { throw failure; });
  assert.throws(() => missing.api.createFromPath('/image.png'), error => error === failure);
  for (const value of [undefined, {}, {pngBase64: '', width: 1, height: 1},
    {pngBase64, width: 0, height: 1}]) {
    assert.throws(() => fixture(() => value).api.createFromBuffer(png), {code: 'ERR_OPERATION_FAILED'});
  }
});

test('oversized input is rejected before native IPC and base64 expansion', () => {
  const oversized = Buffer.alloc(32 * 1024 * 1024 + 1);
  const {api, calls} = fixture(undefined, () => oversized);
  assert.throws(() => api.createFromBuffer(oversized), {code: 'ERR_BUFFER_TOO_LARGE'});
  assert.throws(() => api.createFromPath('/large.png'), {code: 'ERR_BUFFER_TOO_LARGE'});
  assert.throws(() => api.createFromDataURL('x'.repeat(128 * 1024 * 1024 + 1)),
      {code: 'ERR_BUFFER_TOO_LARGE'});
  assert.equal(calls.length, 0);
});

for (const kind of ['main', 'renderer']) {
  test(`${kind}: complete bootstrap exposes nativeImage and uses shared native transport`, () => {
    const native = fixture();
    const context = vm.createContext({
      TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
      setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
      console: {log() {}, warn() {}, error() {}},
      __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
      __xenonOsRelease: '', __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
      __xenonRendererUrlMappings: [], __xenonAppName: 'fixture', __xenonAppVersion: '1',
      __xenonUserAgent: '', __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1,
      __xenonEnv: {}, __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
      __xenonBrowserWindowCall: (_id, command, request) => {
        assert.equal(command, 'electron-api'); return native.call(request.operation, request);
      },
      __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
      location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
      xenonIpcRenderer: {
        getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture'}),
        setDispatchHandler() {},
        sendSync: (channel, request) => {
          assert.equal(channel, '__xenon:electron-api'); return native.call(request.operation, request);
        },
      },
    });
    const filename = path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`);
    vm.runInContext(readBootstrap(filename), context, {filename});
    const api = kind === 'main' ? context.__xenonElectron.nativeImage : context.require('electron').nativeImage;
    const image = api.createFromDataURL('data:image/png;base64,' + pngBase64);
    assert.equal(image.isEmpty(), false);
    assert.deepEqual({...image.getSize()}, {width: 2, height: 1});
    assert.equal(image.toPNG().toString('base64'), pngBase64);
    assert.equal(native.calls[0].operation, 'nativeImage.decode');
  });
}

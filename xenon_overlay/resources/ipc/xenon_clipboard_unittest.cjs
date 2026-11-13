// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function isolated(handlers = {}) {
  const calls = [];
  const call = (operation, details) => {
    calls.push({operation, ...JSON.parse(JSON.stringify(details))});
    return handlers[operation]?.(details);
  };
  const context = vm.createContext({Buffer, Blob, call});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'common/clipboard.js'), 'utf8') +
      ';globalThis.api = createClipboardApi({callSync:call,callAsync:async (...args)=>call(...args),Blob});', context);
  return {calls, ...context.api};
}

test('ClipboardItem snapshots its MIME map and returns real Blob payloads', async () => {
  const {ClipboardItem} = isolated();
  for (const value of [undefined, null, [], '', {}])
    assert.throws(() => new ClipboardItem(value), {code: 'ERR_INVALID_ARG_TYPE'});
  const blob = new Blob([Uint8Array.from([0, 255, 42])], {type: 'image/png'});
  const data = {'text/plain': '中文\0text', 'image/png': blob};
  const item = new ClipboardItem(data);
  data['text/plain'] = 'changed';
  item.types.push('invalid');
  assert.deepEqual(Array.from(item.types), ['text/plain', 'image/png']);
  const text = await item.getType('text/plain');
  assert.ok(text instanceof Blob);
  assert.equal(text.type, 'text/plain');
  assert.equal(await text.text(), '中文\0text');
  assert.equal(await item.getType('image/png'), blob);
  const differentlyTyped = new Blob(['text'], {type: 'text/custom'});
  const originalBlob = await new ClipboardItem({'text/plain': differentlyTyped}).getType('text/plain');
  assert.equal(originalBlob, differentlyTyped);
  assert.equal(originalBlob.type, 'text/custom', 'constructed items preserve supplied Blob MIME');
  await assert.rejects(item.getType('text/html'), /was not found/);
  await assert.rejects(item.getType(42), {code: 'ERR_INVALID_ARG_TYPE'});
});

test('clipboard.write resolves every representation before one native commit', async () => {
  const {clipboard, ClipboardItem, calls} = isolated();
  let release;
  const delayed = new Promise(resolve => { release = resolve; });
  const pending = clipboard.write([
    new ClipboardItem({'text/plain': delayed, 'text/html': '<b>中文</b>'}),
    new ClipboardItem({'image/png': new Blob([Uint8Array.from([0, 255, 42])])}),
  ]);
  assert.equal(typeof pending.then, 'function');
  await Promise.resolve();
  assert.equal(calls.length, 0);
  release('ready');
  await pending;
  assert.deepEqual(calls, [{operation: 'clipboard.writeItems', type: 'clipboard', items: [
    [{mime: 'text/plain', data: Buffer.from('ready').toString('base64')},
      {mime: 'text/html', data: Buffer.from('<b>中文</b>').toString('base64')}],
    [{mime: 'image/png', data: Buffer.from([0, 255, 42]).toString('base64')}],
  ]}]);
});

test('invalid, rejected and unsupported payloads never issue a native write', async () => {
  const {clipboard, ClipboardItem, calls} = isolated();
  const failure = new Error('payload failed');
  for (const value of [42, {}, Uint8Array.from([1])]) {
    await assert.rejects(clipboard.write([new ClipboardItem({'text/plain': 'first'}),
      new ClipboardItem({'text/html': value})]), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  await assert.rejects(clipboard.write([new ClipboardItem({'text/plain': Promise.reject(failure)})]),
      error => error === failure);
  await assert.rejects(clipboard.write([new ClipboardItem({'application/x-custom': 'content'})]),
      {code: 'ERR_NOT_SUPPORTED'});
  await assert.rejects(clipboard.write({text: 'legacy object'}), {code: 'ERR_INVALID_ARG_TYPE'});
  await assert.rejects(clipboard.write([Object.create(ClipboardItem.prototype)]), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls.length, 0);
});

test('read-side ClipboardItems expose actual types and fetch bytes lazily', async () => {
  let bytes = 'before';
  const {clipboard, ClipboardItem, calls} = isolated({
    'clipboard.readTypes': () => ['text/plain'],
    'clipboard.readType': () => Buffer.from(bytes).toString('base64'),
    'clipboard.has': ({mime}) => mime === 'text/plain',
  });
  const items = await clipboard.read();
  assert.equal(items.length, 1);
  assert.ok(items[0] instanceof ClipboardItem);
  assert.deepEqual(Array.from(items[0].types), ['text/plain']);
  assert.equal(calls.length, 1, 'read does not eagerly load payloads');
  bytes = 'after';
  assert.equal(await (await items[0].getType('text/plain')).text(), 'after');
  assert.equal(await clipboard.has('text/plain'), true);
  assert.equal(await clipboard.has('image/png'), false);
  await assert.rejects(clipboard.has(42), {code: 'ERR_INVALID_ARG_TYPE'});
  await assert.rejects(clipboard.write(items), /cannot accept a ClipboardItem returned/);
  await assert.rejects(items[0].getType('text/html'), /was not found/);
  assert.equal(calls.filter(call => call.operation === 'clipboard.writeItems').length, 0);
});

test('empty platform clipboard still returns a read-side item with no invented MIME', async () => {
  const {clipboard} = isolated({'clipboard.readTypes': () => []});
  const [item] = await clipboard.read();
  assert.deepEqual(Array.from(item.types), []);
  await assert.rejects(item.getType('text/plain'), /was not found/);
});

test('clipboard keeps legacy sync calls and propagates native errors', async () => {
  const failure = Object.assign(new Error('host unavailable'), {code: 'EIO'});
  const {clipboard, ClipboardItem, calls} = isolated({
    'clipboard.readText': () => 'native text',
    'clipboard.readHTML': () => '<b>native</b>',
    'clipboard.writeItems': () => { throw failure; },
    'clipboard.readTypes': () => { throw failure; },
    'clipboard.has': () => { throw failure; },
  });
  assert.equal(clipboard.readText(), 'native text');
  assert.equal(clipboard.readHTML(), '<b>native</b>');
  assert.equal(clipboard.writeText('value'), undefined);
  assert.equal(clipboard.writeHTML('<b>value</b>'), undefined);
  assert.equal(clipboard.clear('selection'), undefined);
  assert.equal(calls[4].type, 'selection', 'explicit buffer requests are never redirected');
  await assert.rejects(clipboard.write([new ClipboardItem({'text/plain': 'value'})]), error => error === failure);
  await assert.rejects(clipboard.read(), error => error === failure);
  await assert.rejects(clipboard.has('text/plain'), error => error === failure);
});

for (const kind of ['main', 'renderer']) {
  test(`${kind}: complete bootstrap exports ClipboardItem and invokes the shared clipboard bridge`, async () => {
    const calls = [];
    let nativeBytes = Buffer.from('from native');
    const invoke = request => {
      calls.push(JSON.parse(JSON.stringify(request)));
      if (request.operation === 'clipboard.readTypes') return ['text/plain'];
      if (request.operation === 'clipboard.readType') return nativeBytes.toString('base64');
      if (request.operation === 'clipboard.has') return request.mime === 'text/plain';
    };
    const context = vm.createContext({
      Blob, URL, URLSearchParams, queueMicrotask,
      ...(kind === 'renderer' ? {TextEncoder, TextDecoder} : {}),
      setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
      console: {log() {}, warn() {}, error() {}},
      __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
      __xenonOsRelease: '', __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
      __xenonRendererUrlMappings: [], __xenonAppName: 'fixture', __xenonAppVersion: '1',
      __xenonUserAgent: '', __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1,
      __xenonEnv: {}, __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
      __xenonBrowserWindowCall: (_id, command, request) => {
        assert.equal(command, 'electron-api'); return invoke(request);
      },
      __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
      location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
      xenonIpcRenderer: {
        getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture'}),
        setDispatchHandler() {},
        invoke: async (channel, request) => {
          assert.equal(channel, '__xenon:electron-api'); return invoke(request);
        },
      },
    });
    const filename = path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`);
    vm.runInContext(readBootstrap(filename), context, {filename});
    const electron = kind === 'main' ? context.__xenonElectron : context.require('electron');
    const {clipboard, ClipboardItem} = electron;
    assert.equal(typeof ClipboardItem, 'function');
    const PayloadBlob = kind === 'main' ? context.Blob : Blob;
    await clipboard.write([new ClipboardItem({'text/plain': new PayloadBlob(['real bytes'])})]);
    const [item] = await clipboard.read();
    assert.equal(await (await item.getType('text/plain')).text(), 'from native');
    assert.equal(await clipboard.has('text/plain'), true);
    assert.deepEqual(calls.map(call => call.operation),
        ['clipboard.writeItems', 'clipboard.readTypes', 'clipboard.readType', 'clipboard.has']);
    assert.equal(calls[0].items[0][0].data, Buffer.from('real bytes').toString('base64'));
    if (kind === 'main') {
      assert.notEqual(context.TextEncoder, TextEncoder, 'plain V8 uses its fallback encoder');
      assert.notEqual(context.TextDecoder, TextDecoder, 'plain V8 uses its fallback decoder');
      assert.equal(context.__xenonBuffer.Blob, context.Blob);
    }
    const unicode = 'a\ud800b\udc00中🙂';
    const itemToWrite = new ClipboardItem({'text/plain': unicode});
    assert.equal(await (await itemToWrite.getType('text/plain')).text(),
        await new Blob([unicode]).text());
    await clipboard.write([itemToWrite]);
    assert.equal(calls.at(-1).items[0][0].data, Buffer.from(unicode).toString('base64'));
    nativeBytes = Buffer.from([0xff, 0x61, 0xe2, 0x82]);
    const [invalidUtf8Item] = await clipboard.read();
    const invalidUtf8Blob = await invalidUtf8Item.getType('text/plain');
    assert.equal(await invalidUtf8Blob.text(), await new Blob([nativeBytes]).text());
    assert.deepEqual(Array.from(new Uint8Array(await invalidUtf8Blob.arrayBuffer())), Array.from(nativeBytes));
  });
}

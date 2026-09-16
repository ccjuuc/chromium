// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compare the production Buffer sections with the local Node Buffer oracle.
const assert = require('node:assert/strict');
const {existsSync, readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function createBufferContext(side, nativeDecode = false) {
  const source = readFileSync(path.join(__dirname, `xenon_ipc_${side}_bootstrap.js`), 'utf8');
  const start = source.indexOf('  const nativeToBase64 =');
  const end = source.indexOf(side === 'main' ?
      '  globalThis.Buffer = Buffer;' : '  const utilModule =', start);
  assert.ok(start >= 0 && end > start);
  const context = vm.createContext({TextEncoder, TextDecoder, atob, btoa});
  if (nativeDecode) vm.runInContext(`
    Uint8Array.fromBase64 = value => {
      const binary = atob(value);
      const bytes = Uint8Array.from(binary, char => char.charCodeAt(0));
      globalThis.lastDecodedBytes = bytes;
      return bytes;
    };
  `, context);
  vm.runInContext('const textEncoder = new TextEncoder();\n' +
      'const textDecoder = new TextDecoder();\n' + source.slice(start, end) +
      '\nglobalThis.Buffer = Buffer;', context);
  return context;
}

const cases = {
  'readInt16LE reads signed words from window messages and shared views': `
    const words = [0, 1, 0x7fff, 0x8000, 0xfffe, 0xffff];
    return words.map(word => {
      const source = Buffer.from([9, word & 255, word >>> 8, 9]);
      const view = source.subarray(1, 3);
      return [source.readInt16LE(1), view.readInt16LE(),
        view.readInt16LE(0), view.readInt16LE(0) & 1];
    });
  `,
  'readInt16LE validates offsets and view bounds with Node error codes': `
    const offsets = [undefined, null, '0', {}, 0n, Symbol('offset'),
      -1, -0, 0, 1, 1.5, NaN, Infinity, -Infinity, 3, 4];
    return [0, 1, 2, 4].map(size => {
      const view = Buffer.alloc(10).subarray(2, size + 2);
      return offsets.map(offset => {
        try { return view.readInt16LE(offset); }
        catch (error) { return [error.name, error.code || null]; }
      });
    });
  `,
  'from typed arrays copies and converts elements': `
    const source = new Uint8Array([9, 1, 2, 9]);
    const copied = Buffer.from(source.subarray(1, 3));
    source[1] = 7;
    copied[1] = 8;
    const wide = new Uint16Array([0x1234, 0x5678]);
    const converted = Buffer.from(wide);
    wide[0] = 1;
    let bigint;
    try { Buffer.from(new BigUint64Array([1n])); }
    catch (error) { bigint = error.name; }
    return {source: Array.from(source), copied: Array.from(copied),
      shares: copied.buffer === source.buffer, converted: Array.from(converted),
      signed: Array.from(Buffer.from(new Int16Array([-1, 258]))),
      floating: Array.from(Buffer.from(new Float32Array([1.9, 257.7]))),
      dataView: Array.from(Buffer.from(new DataView(new ArrayBuffer(4)))), bigint};
  `,
  'from ArrayBuffer shares exactly the selected byte range': `
    const storage = new ArrayBuffer(4);
    const bytes = new Uint8Array(storage);
    bytes.set([1, 2, 3, 4]);
    const view = Buffer.from(storage, 1, 2);
    view[0] = 9;
    bytes[2] = 8;
    let conversions = 0;
    const counted = Buffer.from(storage, 0, {valueOf() { ++conversions; return 2; }});
    const ranges = [[-1, 2], [4, 0], [4, 1], [5, 0], [1.9, 2.9],
      [0, -1], [0, NaN], [NaN, 2], [-0.5, 2], [1, Infinity]];
    return {shares: view.buffer === storage, offset: view.byteOffset,
      values: Array.from(view), bytes: Array.from(bytes), conversions,
      counted: counted.length, ranges: ranges.map(([offset, length]) => {
        try { const value = Buffer.from(storage, offset, length);
          return [value.byteOffset, value.length]; }
        catch (error) { return [error.name, error.code || null]; }
      })};
  `,
  'slice returns a branded shared view with relative range semantics': `
    const source = Buffer.from([1, 2, 3, 4]);
    const view = source.slice(1, 3);
    view[0] = 9;
    source[2] = 8;
    return {shares: source.buffer === view.buffer, branded: Buffer.isBuffer(view),
      offset: view.byteOffset - source.byteOffset, source: Array.from(source),
      values: Array.from(view), negative: Array.from(source.slice(-2)),
      empty: source.slice(3, 1).length};
  `,
  'isBuffer distinguishes plain typed arrays': `
    const buffer = Buffer.from([1, 2]);
    return [Buffer.isBuffer(buffer), Buffer.isBuffer(buffer.subarray(1)),
      Buffer.isBuffer(new Uint8Array(2)), Buffer.isBuffer(new Uint16Array(2)),
      Buffer.isBuffer(new Uint8Array(buffer.buffer)), Buffer.isBuffer({}),
      Buffer.isBuffer(null)];
  `,
  'byteLength matches encoded lengths without allocating a Buffer': `
    const strings = ['', 'ascii', '中文', '\\u00ff', '\\u0800', '💩',
      '\\ud800', '\\udfff', '\\ud800x', '\\ud800\\ud800\\udfff',
      'abc', 'ab=x', 'a b\\nc', 'a===', 'abcd===='];
    const encodings = [undefined, null, 1, {}, 'unknown', 'utf8', 'UTF-8',
      'ascii', 'ASCII', 'latin1', 'binary', 'utf16le', 'utf-16le', 'ucs2',
      'UCS-2', 'hex', 'HEX', 'base64', 'base64url'];
    const original = Buffer.from;
    Buffer.from = () => { throw new Error('byteLength must not allocate a Buffer'); };
    try { return strings.map(value => encodings.map(encoding =>
      Buffer.byteLength(value, encoding))); }
    finally { Buffer.from = original; }
  `,
  'byteLength measures view bytes and rejects unsupported inputs': `
    const storage = new ArrayBuffer(16);
    const inputs = [Buffer.from([1, 2, 3]), new Uint8Array(storage, 3, 4),
      new Uint16Array(storage, 2, 3), new DataView(storage, 5, 7), storage,
      new SharedArrayBuffer(9), undefined, null, 1, true, {}, [],
      new String('abc'), {byteLength: 5}, {[Symbol.toStringTag]: 'ArrayBuffer'}];
    return inputs.map(value => {
      try { return Buffer.byteLength(value); }
      catch (error) { return [error.name, error.code || null]; }
    });
  `,
  'readUIntLE reads unsigned one to six byte integers and its Uint alias': `
    const source = Buffer.from([9, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 9]);
    const view = source.subarray(1, 7);
    return {values: [1, 2, 3, 4, 5, 6].map(length =>
      [source.readUIntLE(1, length), view.readUIntLE(0, length)]),
      windowHandle: Buffer.from([0xff, 0xff, 0xff, 0xff]).readUIntLE(0, 4),
      maximum: Buffer.alloc(6, 255).readUIntLE(0, 6),
      alias: Buffer.prototype.readUIntLE === Buffer.prototype.readUintLE,
      aliasValue: view.readUintLE(0, 6)};
  `,
  'readUIntLE validates width offset and subview bounds with Node error codes': `
    const offsets = [undefined, null, '0', {}, 0n, -1, -0, 0, 1, 1.5, NaN,
      Infinity, -Infinity, 7, 8];
    const lengths = [undefined, null, '1', {}, -1, 0, 1, 1.5, 2, 3, 6,
      7, 8, NaN, Infinity];
    return [0, 2, 8].map(size => {
      const source = Buffer.alloc(12).subarray(1, 1 + size);
      return offsets.map(offset => lengths.map(length => {
        try { return source.readUIntLE(offset, length); }
        catch (error) { return [error.name, error.code || null]; }
      }));
    });
  `,
  'readUIntLE rejects BigInt and Symbol widths as TypeErrors': `
    return [1n, Symbol('width')].map(width => {
      try { return Buffer.alloc(8).readUIntLE(0, width); }
      catch (error) { return error.name; }
    });
  `,
};

for (const side of ['main', 'renderer']) {
  for (const [name, source] of Object.entries(cases)) {
    test(`${side} Buffer ${name}`, () => {
      const actual = vm.runInContext(`(() => {${source}})()`, createBufferContext(side));
      const expected = new Function('Buffer', source)(Buffer);
      assert.deepEqual(JSON.parse(JSON.stringify(actual)), expected);
    });
  }
  test(`${side} base64 adopts its freshly decoded storage without another copy`, () => {
    const context = createBufferContext(side, true);
    assert.equal(vm.runInContext(`
      const decoded = Buffer.from('AQID', 'base64');
      Buffer.isBuffer(decoded) && decoded.buffer === lastDecodedBytes.buffer &&
          decoded.byteOffset === lastDecodedBytes.byteOffset && decoded.length === 3;
    `, context), true);
  });
  test(`${side} byteLength accepts backing stores and views from another realm`, () => {
    const context = createBufferContext(side);
    context.inputs = [new ArrayBuffer(9), new SharedArrayBuffer(11),
      new Uint16Array(3), new DataView(new ArrayBuffer(8), 1, 4)];
    const result = vm.runInContext('inputs.map(value => Buffer.byteLength(value))', context);
    assert.deepEqual(Array.from(result), context.inputs.map(value => Buffer.byteLength(value)));
  });
}

test('renderer Buffer.isEncoding matches Node aliases, case and non-string rejection', () => {
  const context = createBufferContext('renderer');
  const fixture = `
    const encodings = ['utf8', 'utf-8', 'utf16le', 'utf-16le', 'ucs2', 'ucs-2',
      'latin1', 'binary', 'ascii', 'base64', 'base64url', 'hex',
      '', 'utf16', 'utf-16', 'utf16be', 'base64-url', 'buffer', 'raw',
      'unicode', 'utf32', ' utf8', 'utf8 ', 'utf8\\n', 'utf8\\0'];
    let conversions = 0;
    const nonStrings = [undefined, null, 0, 1, true, false, 1n, Symbol('utf8'),
      new String('utf8'), [], ['utf8'], {},
      {toString() { ++conversions; return 'utf8'; },
       [Symbol.toPrimitive]() { ++conversions; return 'utf8'; }},
      new Proxy({}, {get() { throw new Error('Do not inspect non-strings'); }})];
    const borrowed = Buffer.isEncoding;
    return {encodings: encodings.flatMap(value =>
      [value, value.toUpperCase(), value.replace(/[a-z]/g, (char, i) =>
        i % 2 ? char.toUpperCase() : char)]).map(value => Buffer.isEncoding(value)),
      nonStrings: nonStrings.map(value => Buffer.isEncoding(value)),
      conversions, borrowed: borrowed.call(null, 'utf8')};
  `;
  const actual = vm.runInContext(`(() => {${fixture}})()`, context);
  const expected = new Function('Buffer', fixture)(Buffer);
  assert.deepEqual(JSON.parse(JSON.stringify(actual)), expected);
});

const syncKitBundle = process.env.XENON_TEST_TH_SYNC_KIT ||
    'F:/thunder_2025/app/node_modules/@xbase/electron_sync_kit/dist/cjs/development/index.js';

test('renderer Buffer supports the actual TH MQTT Writable UTF-8 path',
    {skip: !existsSync(syncKitBundle)}, () => {
  const source = readFileSync(syncKitBundle, 'utf8');
  const entry = source.indexOf('var __webpack_exports__ = {};');
  assert.ok(entry > 0, 'TH sync kit must expose its webpack module table before the entry');
  // Define the installed bundle's module table without executing its app
  // entry. Only the Writable dependency is loaded, with no MQTT connection.
  const fixture = source.slice(0, entry) +
      'globalThis.__loadWritableTestModule = __webpack_require__;})();';
  const BufferImpl = createBufferContext('renderer').Buffer;
  function loadWritable(BufferOverride) {
    const allowed = new Set(['assert', 'events', 'stream', 'util']);
    const context = vm.createContext({
      Buffer: BufferOverride,
      process: {nextTick: process.nextTick, version: process.version,
        versions: process.versions, env: {}},
      setTimeout, clearTimeout, setInterval, clearInterval, queueMicrotask,
      TextEncoder, TextDecoder, AbortController,
      require(id) {
        if (id === 'buffer') return {Buffer: BufferOverride};
        assert.ok(allowed.has(id), 'Unexpected TH Writable fixture external: ' + id);
        return require(id);
      },
    });
    context.global = context;
    vm.runInContext(fixture, context, {filename: syncKitBundle});
    return context.__loadWritableTestModule(
      '../../node_modules/.pnpm/readable-stream@4.7.0/node_modules/readable-stream/lib/internal/streams/writable.js');
  }

  const MissingEncoding = new Proxy(BufferImpl, {get(target, key, receiver) {
    return key === 'isEncoding' ? undefined : Reflect.get(target, key, receiver);
  }});
  const OldWritable = loadWritable(MissingEncoding);
  const oldStream = new OldWritable({write(_chunk, _encoding, callback) { callback(); }});
  assert.throws(() => oldStream.write('fixture', 'utf8'), /isEncoding is not a function/);

  const Writable = loadWritable(BufferImpl);
  const chunks = [];
  const stream = new Writable({write(chunk, encoding, callback) {
    assert.ok(BufferImpl.isBuffer(chunk));
    assert.equal(encoding, 'buffer');
    chunks.push(Array.from(chunk));
    callback();
  }});
  assert.equal(stream.write('同步客户端 / fixture', 'utf8'), true);
  assert.equal(stream.setDefaultEncoding('UTF-8'), stream);
  assert.equal(stream.write('第二段 / continuation'), true);
  assert.throws(() => stream.setDefaultEncoding('not-an-encoding'),
      error => error.code === 'ERR_UNKNOWN_ENCODING');
  assert.deepEqual(chunks, ['同步客户端 / fixture', '第二段 / continuation']
      .map(value => Array.from(Buffer.from(value, 'utf8'))));
  stream.end();
});

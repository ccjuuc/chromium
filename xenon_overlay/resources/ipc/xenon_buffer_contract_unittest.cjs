// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compare the production Buffer sections with the local Node Buffer oracle.
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const {existsSync, readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function createBufferContext(side, nativeDecode = false) {
  const source = readBootstrapPart(`${side}/buffer.js`);
  const context = vm.createContext({TextEncoder, TextDecoder, atob, btoa});
  if (nativeDecode) vm.runInContext(`
    Uint8Array.fromBase64 = value => {
      const binary = atob(value);
      const bytes = Uint8Array.from(binary, char => char.charCodeAt(0));
      globalThis.lastDecodedBytes = bytes;
      return bytes;
    };
  `, context);
  vm.runInContext(source +
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

const rendererEncodingCases = {
  'from encodes every advertised alias and case with Node bytes': `
    const strings = ['', 'plain', 'éÿ€中文💩', '\\0\\ufeff\\ud800x\\udfff',
      '\\ud800\\ud800\\udfff', '0123456789abcdefABCDEF', '1a7', '1g',
      '1a 2b', '0x12', '\\u0131f', '1\\u0161', '\\u0141\\u0142',
      'YWJj', 'YW!Jj', 'Y W\\nJj', 'Y-W_', 'YWJj====', 'YWJj=AAAA',
      'a', 'ab', 'abc', 'abcd', 'ab=x', 'abcd====', '\\u0141A',
      'éAA', 'YW\\ud800Jj', '\\u013dAAAA'];
    return strings.map(value => aliases.map(encoding =>
      Array.from(Buffer.from(value, encoding))));
  `,
  'from defaults non-string encodings without coercing them': `
    let conversions = 0;
    const encodings = [undefined, null, false, true, 0, 1, NaN, 0n, 1n,
      Symbol('hex'), {}, [], ['hex'], new String('hex'),
      {toString() { ++conversions; return 'hex'; }},
      new Proxy({}, {get() { throw new Error('encoding was inspected'); }}),
      '', 'utf8', 'utf-8', 'hex', 'utf16le', 'made-up', 'raw', 'utf16be'];
    return {values: ['', 'é', 'ab'].map(value => encodings.map(encoding =>
      attempt(() => Array.from(Buffer.from(value, encoding))))), conversions};
  `,
  'toString handles UTF-8 BOM malformed bytes ASCII Latin-1 and raw UTF-16': `
    const inputs = [[], [0, 65, 127, 128, 159, 233, 255], [239, 187, 191, 65],
      [192, 128], [224, 128, 175], [237, 160, 128], [240, 159, 146, 169],
      [244, 144, 128, 128], [226, 130], [226, 65, 130], [255, 254, 65, 0],
      [0, 216], [0, 220], [0, 216, 0, 220], [65, 0, 66],
      Array.from({length: 256}, (_, i) => i)];
    return inputs.map(bytes => aliases.map(encoding =>
      Buffer.from(bytes).toString(encoding)));
  `,
  'toString clamps ranges within active views before validating encoding': `
    const buffer = Buffer.from([9, 0xef, 0xbb, 0xbf, 0x41, 0xc3, 0xa9, 9])
      .subarray(1, 7);
    const ranges = [[undefined, undefined], [-3, undefined], [0, -1],
      [1, 4], [2.9, 5.9], [0, 99], [99, 100], [3, 1], [NaN, NaN],
      [null, undefined], ['1', '5'], [-Infinity, Infinity], [1n, 3]];
    return ranges.map(([start, end]) => [...aliases, '', null, 'bad']
      .map(encoding => attempt(() => buffer.toString(encoding, start, end))));
  `,
  'toString write and byteLength apply their encoding coercion rules': `
    let conversions = 0;
    const encodings = [undefined, null, false, true, 0, 1, NaN, 0n, 1n,
      Symbol('hex'), {}, [], ['hex'], new String('hex'), new String('utf16le'),
      {toString() { ++conversions; return 'latin1'; }},
      {[Symbol.toPrimitive]() { ++conversions; return 'utf16le'; }},
      '', 'bad', 'utf-8', 'UCS-2'];
    const output = encodings.map(encoding => [
      attempt(() => Buffer.from([233, 0]).toString(encoding)),
      attempt(() => { const buffer = Buffer.alloc(6, 33);
        const count = buffer.write('é', 0, 6, encoding);
        return [count, Array.from(buffer)]; }),
      attempt(() => Buffer.byteLength('é', encoding)),
      attempt(() => Buffer.byteLength('', encoding))]);
    return {output, conversions};
  `,
  'write respects encoded character boundaries and leaves adjacent view bytes intact': `
    const values = ['é中文💩', '\\ufeff\\ud800x\\udfff', '61f', '1a7', 'ab=x', 'Y-W_'];
    return values.map(value => aliases.map(encoding =>
      Array.from({length: 10}, (_, length) => {
        const backing = Buffer.alloc(13, 33);
        const view = backing.subarray(2, 11);
        const count = view.write(value, 1, Math.min(length, 8), encoding);
        return [count, Array.from(backing)];
      })));
  `,
  'write validates overloads offsets lengths values and unknown codecs': `
    const args = [['é'], ['é', 'latin1'], ['é', 1, 'utf16le'],
      ['é', undefined, 0, 'latin1'], ['é', 'latin1', 2],
      ['é', 0, 0, 'bad'], ['', 0, 0, 'bad'], [1], [null], [new String('é')],
      ['é', null], ['é', -1], ['é', 1.5], ['é', NaN], ['é', Infinity],
      ['é', 7], ['é', 6], ['é', 0, -1], ['é', 0, 7], ['é', 0, null],
      ['é', 0, 1.5], ['é', 0, NaN], ['é', 5, 6, 'utf8']];
    return args.map(args => attempt(() => {
      const buffer = Buffer.alloc(6, 33);
      return [buffer.write(...args), Array.from(buffer)];
    }));
  `,
  'fill and alloc repeat encoded bytes for every alias': `
    return ['', 'é', '中文💩', 'ab', 'abcd', 'YWJj', '\\ud800'].map(value =>
      aliases.map(encoding => [
        attempt(() => Array.from(Buffer.alloc(9, value, encoding))),
        attempt(() => { const backing = Buffer.alloc(13, 33);
          const view = backing.subarray(2, 11);
          const result = view.fill(value, 1, 8, encoding);
          return [result === view, Array.from(backing)]; }),
        attempt(() => Array.from(Buffer.alloc(9).fill(value, encoding))),
        attempt(() => Array.from(Buffer.alloc(9).fill(value, 1, encoding)))]));
  `,
  'fill validates encodings and ranges and rejects nonempty undecodable patterns': `
    const args = [['a', 0, 6, 'bad'], ['', 0, 6, 'bad'],
      ['a', 6, 6, 'bad'], ['a', 0, 6, false], ['a', 0, 6, 0],
      ['a', 0, 6, null], ['a', 0, 6, Symbol('utf8')],
      ['a', 0, 6, {}], ['ab', 0, 6, ''], ['ab', 0, 6, 'hex'],
      ['g', 0, 6, 'hex'], ['a', 0, 6, 'base64'], ['a', 0, 0, 'base64'],
      ['é', undefined, 2, 'latin1'], ['é', null], ['a', -1],
      ['a', 1.5], ['a', NaN], ['a', Infinity], ['a', 7],
      ['a', 0, 7], ['a', 0, null], ['a', 0, -1], ['a', 4, 2],
      [17, 0, 6, 'bad']];
    return args.map(args => attempt(() => Array.from(Buffer.alloc(6, 33).fill(...args))));
  `,
  'fill uses raw view bytes and safely repeats overlapping patterns': `
    const inputs = [Buffer.from([2, 3]), new Uint8Array([5, 6, 7]),
      new Uint16Array([0x1234, 0x5678]), new DataView(new Uint8Array([2, 4, 6]).buffer),
      new Uint8Array(), {}, undefined, null, true];
    const output = inputs.map(value => attempt(() => Array.from(Buffer.alloc(7).fill(value))));
    const overlap = [0, 1, 2, 3].map(start => {
      const buffer = Buffer.from([1, 2, 3, 4, 5, 6, 7]);
      buffer.fill(buffer.subarray(1, 4), start, 7);
      return Array.from(buffer);
    });
    return {output, overlap};
  `,
};

for (const [name, source] of Object.entries(rendererEncodingCases)) {
  test(`renderer Buffer ${name}`, () => {
    const fixture = `
      const aliases = ['utf8', 'utf-8', 'latin1', 'binary', 'ascii', 'utf16le',
        'utf-16le', 'ucs2', 'ucs-2', 'hex', 'base64', 'base64url']
        .flatMap(name => [name, name.toUpperCase()]);
      function attempt(callback) {
        try { return callback(); }
        catch (error) { return [error.name, error.code || null]; }
      }
      ${source}
    `;
    const actual = vm.runInContext(`(() => {${fixture}})()`, createBufferContext('renderer'));
    const expected = new Function('Buffer', fixture)(Buffer);
    assert.deepEqual(JSON.parse(JSON.stringify(actual)), expected);
  });
}

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

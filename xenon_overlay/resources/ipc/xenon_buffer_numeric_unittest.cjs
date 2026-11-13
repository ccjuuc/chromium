// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function implementation(side) {
  const context = vm.createContext({TextEncoder, TextDecoder, atob, btoa});
  vm.runInContext(readBootstrapPart('common/buffer_numeric.js') + '\n' +
      readBootstrapPart('common/buffer.js') + '\n' +
      readBootstrapPart('common/promisify.js') + '\n' +
      readBootstrapPart(side + '/buffer.js') + '\nthis.TestBuffer = Buffer;', context);
  return context.TestBuffer;
}

function outcome(BufferClass, method, size, args) {
  const backing = BufferClass.from([91, ...Array(size).fill(0xa5), 92]);
  const view = backing.subarray(1, size + 1);
  let result;
  try {
    result = view[method](...args);
    if (typeof result === 'bigint') result = result.toString() + 'n';
    else if (Number.isNaN(result)) result = 'NaN';
    else if (Object.is(result, -0)) result = '-0';
  } catch (error) {
    result = {name: error.name, code: error.code || null};
  }
  return {result, bytes: Array.from(backing)};
}

function compare(BufferClass, method, size, args) {
  assert.deepEqual(outcome(BufferClass, method, size, args),
      outcome(Buffer, method, size, args),
      `${method}, size=${size}, arguments=${args.map(value => String(value)).join(',')}`);
}

for (const side of ['main', 'renderer']) {
  const CompatBuffer = implementation(side);
  test(`${side} fixed integer reads and writes match Node across values and subview bounds`, () => {
    const offsets = [undefined, null, '0', {}, 0n, Symbol('offset'),
      -1, -0, 0, 1, 1.5, NaN, Infinity, -Infinity, 8];
    for (const signed of [false, true]) for (const bytes of [1, 2, 4]) {
      const limit = 2 ** (bytes * 8 - (signed ? 1 : 0));
      const values = [undefined, null, false, true, '12', {}, NaN, Infinity, -Infinity,
        -1, -0.5, 0, 0.5, 1, limit - 1, limit, signed ? -limit : -1,
        signed ? -limit - 1 : -1, 1n, Symbol('value')];
      for (const endian of bytes === 1 ? [''] : ['LE', 'BE']) {
        const suffix = (signed ? 'Int' : 'UInt') + bytes * 8 + endian;
        for (const size of [0, bytes - 1, bytes, bytes + 1, 9]) {
          for (const offset of offsets) {
            compare(CompatBuffer, 'read' + suffix, size, [offset]);
            for (const value of values)
              compare(CompatBuffer, 'write' + suffix, size, [value, offset]);
          }
        }
      }
    }
  });
  test(`${side} variable one-to-six-byte integers preserve 48-bit values and validation order`, () => {
    const widths = [undefined, null, '1', {}, 0, 1, 2, 3, 4, 5, 6, 7, 1.5, NaN, Infinity];
    const offsets = [undefined, null, '0', -1, 0, 1, 0.5, NaN, Infinity, 8];
    for (const signed of [false, true]) for (const endian of ['LE', 'BE']) {
      const suffix = (signed ? 'Int' : 'UInt') + endian;
      for (const size of [0, 2, 8]) for (const offset of offsets) for (const width of widths) {
        compare(CompatBuffer, 'read' + suffix, size, [offset, width]);
        for (const value of [-0.5, 0, NaN, '1', 0x123456789abc, -0x123456789abc])
          compare(CompatBuffer, 'write' + suffix, size, [value, offset, width]);
      }
      for (let width = 1; width <= 6; ++width) {
        const limit = 2 ** (width * 8 - (signed ? 1 : 0));
        for (const value of [limit - 1, limit, -limit, -limit - 1, 0xffffffff, 0x100000000])
          compare(CompatBuffer, 'write' + suffix, 8, [value, 1, width]);
      }
    }
  });
  test(`${side} BigInt 64-bit methods enforce type range and view bounds`, () => {
    const values = [undefined, null, false, true, 0, 1, -1, NaN, Infinity,
      '1', '18446744073709551616', {}, Symbol('value'), 0n, 1n, -1n,
      -(1n << 63n), (1n << 63n) - 1n, 1n << 63n, (1n << 64n) - 1n, 1n << 64n];
    for (const signed of [false, true]) for (const endian of ['LE', 'BE']) {
      const suffix = 'Big' + (signed ? 'Int' : 'UInt') + '64' + endian;
      for (const size of [0, 7, 8, 9]) for (const offset of [undefined, null, '0', -1, 0, 1, 0.5, NaN, Infinity]) {
        compare(CompatBuffer, 'read' + suffix, size, [offset]);
        for (const value of values)
          compare(CompatBuffer, 'write' + suffix, size, [value, offset]);
      }
    }
  });
  test(`${side} floating point endian methods use Node coercion and exact view bounds`, () => {
    for (const type of ['Float', 'Double']) for (const endian of ['LE', 'BE']) {
      const width = type === 'Float' ? 4 : 8;
      for (const size of [0, width - 1, width, width + 1]) {
        for (const offset of [undefined, null, '0', -1, 0, 1, 0.5, NaN, Infinity]) {
          compare(CompatBuffer, 'read' + type + endian, size, [offset]);
          for (const value of [undefined, null, '1.5', -0, -1.5, NaN, Infinity, -Infinity, 1n])
            compare(CompatBuffer, 'write' + type + endian, size, [value, offset]);
        }
      }
    }
  });
  test(`${side} Uint aliases preserve function identity and method descriptors`, () => {
    const methods = Object.getOwnPropertyNames(Buffer.prototype)
        .filter(name => /^(read|write)(Big)?Uint/.test(name));
    for (const alias of methods) {
      const canonical = alias.replace('Uint', 'UInt');
      assert.equal(CompatBuffer.prototype[alias], CompatBuffer.prototype[canonical]);
      const actual = Object.getOwnPropertyDescriptor(CompatBuffer.prototype, alias);
      const expected = Object.getOwnPropertyDescriptor(Buffer.prototype, alias);
      for (const key of ['enumerable', 'writable', 'configurable'])
        assert.equal(actual[key], expected[key], alias + '.' + key);
    }
  });
  test(`${side} write-file-atomic digest words can be read as big-endian unsigned integers`, () => {
    const digest = require('node:crypto').createHash('sha256').update('atomic save').digest();
    const copy = CompatBuffer.from(digest);
    for (let i = 0; i < digest.length; i += 4)
      assert.equal(copy.readUInt32BE(i), digest.readUInt32BE(i));
  });
}

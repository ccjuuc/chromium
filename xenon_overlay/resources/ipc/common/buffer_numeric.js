// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Both execution environments expose the same Node integer/IEEE754 contract.
// Bounds are relative to the Buffer view, never its larger backing allocation.
function installBufferNumericMethods(BufferClass) {
  const failure = (Type, code, message) => {
    throw Object.assign(new Type(message), {code});
  };
  function number(value, name) {
    if (typeof value !== 'number')
      failure(TypeError, 'ERR_INVALID_ARG_TYPE', `${name} must be a number`);
  }
  function bounds(buffer, offset, width) {
    number(offset, 'offset');
    if (buffer[offset] !== undefined && buffer[offset + width - 1] !== undefined)
      return;
    if (Math.floor(offset) !== offset)
      failure(RangeError, 'ERR_OUT_OF_RANGE', 'offset must be an integer');
    if (buffer.length < width)
      failure(RangeError, 'ERR_BUFFER_OUT_OF_BOUNDS', 'Buffer is too small');
    failure(RangeError, 'ERR_OUT_OF_RANGE', 'offset is outside the Buffer');
  }
  function width(value) {
    number(value, 'byteLength');
    if (!Number.isInteger(value) || value < 1 || value > 6)
      failure(RangeError, 'ERR_OUT_OF_RANGE', 'byteLength must be from 1 to 6');
    return value;
  }
  function range(value, minimum, maximum) {
    if (value > maximum || value < minimum)
      failure(RangeError, 'ERR_OUT_OF_RANGE', 'value is outside the integer range');
  }
  function read(buffer, offset, size, littleEndian, signed) {
    bounds(buffer, offset, size);
    let value = 0;
    for (let i = 0; i < size; ++i)
      value = value * 256 + buffer[offset + (littleEndian ? size - i - 1 : i)];
    return signed && value >= 2 ** (size * 8 - 1) ? value - 2 ** (size * 8) : value;
  }
  function write(buffer, value, offset, size, littleEndian, signed) {
    value = +value;
    if (size === 1) number(offset, 'offset');
    const limit = 2 ** (size * 8 - (signed ? 1 : 0));
    range(value, signed ? -limit : 0, limit - 1);
    bounds(buffer, offset, size);
    // Node truncates each 32-bit word; the high word of 5/6-byte values uses
    // floor, including negative fractional values. Do not narrow to uint32.
    const low = value >>> 0;
    const high = Math.floor(value / 2 ** 32);
    for (let i = 0; i < size; ++i) {
      buffer[offset + (littleEndian ? i : size - i - 1)] =
          i < 4 ? low >>> (i * 8) : high >>> ((i - 4) * 8);
    }
    return offset + size;
  }
  function define(name, fn, alias) {
    Object.defineProperty(fn, 'name', {value: name, configurable: true});
    const descriptor = {value: fn, configurable: true, writable: true, enumerable: true};
    Object.defineProperty(BufferClass.prototype, name, descriptor);
    if (alias) Object.defineProperty(BufferClass.prototype, alias, descriptor);
  }
  for (const signed of [false, true]) {
    const type = signed ? 'Int' : 'UInt';
    for (const size of [1, 2, 4]) {
      for (const littleEndian of size === 1 ? [true] : [true, false]) {
        const suffix = type + size * 8 + (size === 1 ? '' : littleEndian ? 'LE' : 'BE');
        define('read' + suffix, function(offset = 0) {
          return read(this, offset, size, littleEndian, signed);
        }, signed ? undefined : 'read' + suffix.replace('UInt', 'Uint'));
        define('write' + suffix, function(value, offset = 0) {
          return write(this, value, offset, size, littleEndian, signed);
        }, signed ? undefined : 'write' + suffix.replace('UInt', 'Uint'));
      }
    }
    for (const littleEndian of [true, false]) {
      const suffix = type + (littleEndian ? 'LE' : 'BE');
      define('read' + suffix, function(offset, byteLength) {
        if (offset === undefined) number(offset, 'offset');
        return read(this, offset, width(byteLength), littleEndian, signed);
      }, signed ? undefined : 'read' + suffix.replace('UInt', 'Uint'));
      define('write' + suffix, function(value, offset, byteLength) {
        return write(this, value, offset, width(byteLength), littleEndian, signed);
      }, signed ? undefined : 'write' + suffix.replace('UInt', 'Uint'));

      const bigSuffix = 'Big' + type + '64' + (littleEndian ? 'LE' : 'BE');
      define('read' + bigSuffix, function(offset = 0) {
        bounds(this, offset, 8);
        let value = 0n;
        for (let i = 0; i < 8; ++i)
          value = (value << 8n) + BigInt(this[offset + (littleEndian ? 7 - i : i)]);
        return signed ? BigInt.asIntN(64, value) : value;
      }, signed ? undefined : 'read' + bigSuffix.replace('UInt', 'Uint'));
      define('write' + bigSuffix, function(value, offset = 0) {
        const limit = 1n << (signed ? 63n : 64n);
        range(value, signed ? -limit : 0n, limit - 1n);
        bounds(this, offset, 8);
        // Bitwise BigInt conversion deliberately rejects Numbers and strings,
        // which DataView's ToBigInt coercion would otherwise accept.
        let remaining = value & 0xffffffffffffffffn;
        for (let i = 0; i < 8; ++i) {
          this[offset + (littleEndian ? i : 7 - i)] = Number(remaining & 255n);
          remaining >>= 8n;
        }
        return offset + 8;
      }, signed ? undefined : 'write' + bigSuffix.replace('UInt', 'Uint'));
    }
  }
  for (const [type, size] of [['Float', 4], ['Double', 8]]) {
    const method = 'Float' + size * 8;
    for (const littleEndian of [true, false]) {
      const suffix = type + (littleEndian ? 'LE' : 'BE');
      define('read' + suffix, function(offset = 0) {
        bounds(this, offset, size);
        return new DataView(this.buffer, this.byteOffset, this.byteLength)
            ['get' + method](offset, littleEndian);
      });
      define('write' + suffix, function(value, offset = 0) {
        value = +value;
        bounds(this, offset, size);
        new DataView(this.buffer, this.byteOffset, this.byteLength)
            ['set' + method](offset, value, littleEndian);
        return offset + size;
      });
    }
  }
}

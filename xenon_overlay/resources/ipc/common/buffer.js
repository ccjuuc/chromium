// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The main and utility V8 contexts start without Web text codecs. Share the
// bounded encoder between Buffer.write and util.TextEncoder without encoding
// the entire source or writing a partial UTF-8 character into the destination.
const utf8DestinationTag = Object.getOwnPropertyDescriptor(
    Object.getPrototypeOf(Uint8Array.prototype), Symbol.toStringTag).get;
function encodeUtf8Into(source, destination) {
  if (typeof source !== 'string') {
    throw Object.assign(new TypeError('source must be a string'),
                        {code: 'ERR_INVALID_ARG_TYPE'});
  }
  if (!ArrayBuffer.isView(destination) ||
      utf8DestinationTag.call(destination) !== 'Uint8Array') {
    throw Object.assign(new TypeError('destination must be a Uint8Array'),
                        {code: 'ERR_INVALID_ARG_TYPE'});
  }
  let read = 0, written = 0;
  while (read < source.length) {
    let point = source.charCodeAt(read), units = 1;
    if (point >= 0xd800 && point <= 0xdbff) {
      const low = source.charCodeAt(read + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00;
        units = 2;
      } else point = 0xfffd;
    } else if (point >= 0xdc00 && point <= 0xdfff) point = 0xfffd;
    const bytes = point < 0x80 ? 1 : point < 0x800 ? 2 : point < 0x10000 ? 3 : 4;
    if (written + bytes > destination.length) break;
    if (bytes === 1) destination[written++] = point;
    else {
      if (bytes === 2) destination[written++] = 0xc0 | point >> 6;
      else {
        if (bytes === 3) destination[written++] = 0xe0 | point >> 12;
        else {
          destination[written++] = 0xf0 | point >> 18;
          destination[written++] = 0x80 | point >> 12 & 63;
        }
        destination[written++] = 0x80 | point >> 6 & 63;
      }
      destination[written++] = 0x80 | point & 63;
    }
    read += units;
  }
  return {read, written};
}

function encodeUtf8(source = '') {
  // Template conversion follows the Web codec's string conversion, including
  // rejecting Symbols, while preserving false, zero, null and lone surrogates.
  source = `${source}`;
  const bytes = new Uint8Array(source.length * 3);
  const {written} = encodeUtf8Into(source, bytes);
  return written === bytes.length ? bytes : bytes.slice(0, written);
}

function createBufferClass() {
  // Plain V8 main contexts do not necessarily expose the Web text codecs.
  // Keep the Buffer fallback independent of globals installed later in startup.
  const textEncoder = typeof TextEncoder === 'function' ? new TextEncoder() : {
    encode: encodeUtf8,
    encodeInto: encodeUtf8Into,
  };
  // Buffer treats a UTF-8 BOM as content, unlike TextDecoder's default.
  const textDecoder = typeof TextDecoder === 'function' ?
    new TextDecoder('utf-8', {ignoreBOM: true}) : {
      decode(bytes) {
        const parts = [];
        let text = '', point = 0, remaining = 0, lower = 0x80, upper = 0xbf;
        for (let i = 0; i < bytes.length; ++i) {
          const byte = bytes[i];
          if (remaining) {
            if (byte < lower || byte > upper) {
              text += '\ufffd';
              remaining = 0;
              --i; // The invalid continuation can start the next character.
            } else {
              point = point << 6 | byte & 63;
              if (--remaining === 0) text += String.fromCodePoint(point);
            }
            lower = 0x80;
            upper = 0xbf;
          } else if (byte < 0x80) text += String.fromCharCode(byte);
          else if (byte >= 0xc2 && byte <= 0xf4) {
            remaining = byte < 0xe0 ? 1 : byte < 0xf0 ? 2 : 3;
            point = byte & (0x7f >> remaining + 1);
            lower = byte === 0xe0 ? 0xa0 : byte === 0xf0 ? 0x90 : 0x80;
            upper = byte === 0xed ? 0x9f : byte === 0xf4 ? 0x8f : 0xbf;
          } else text += '\ufffd';
          if (text.length >= 8192) { parts.push(text); text = ''; }
        }
        if (remaining) text += '\ufffd';
        parts.push(text);
        return parts.join('');
      },
    };
  const nativeToBase64 = Uint8Array.prototype.toBase64;
  const nativeFromBase64 = Uint8Array.fromBase64;
  // Some older JS hosts expose an experimental API that ignores byteOffset.
  const nativeBase64HandlesViews = !nativeToBase64 ||
      nativeToBase64.call(new Uint8Array([0, 255]).subarray(1)) === '/w==';
  const arrayBufferByteLength = Object.getOwnPropertyDescriptor(
      ArrayBuffer.prototype, 'byteLength').get;
  const sharedArrayBufferByteLength = typeof SharedArrayBuffer === 'function' ?
      Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, 'byteLength').get : null;

  function decodeBase64(value) {
    if (nativeFromBase64) {
      try {
        return nativeFromBase64(value);
      } catch (error) {
        if (!(error instanceof SyntaxError)) throw error;
      }
    }
    // Node Buffer accepts both alphabets, ignores non-alphabet characters and
    // stops at padding. Keep canonical input on V8's allocation-efficient path.
    let normalized = value.replace(/[^\x00-\xff]/g,
        char => String.fromCharCode(char.charCodeAt(0) & 0xff))
        .split('=', 1)[0].replace(/-/g, '+')
        .replace(/_/g, '/').replace(/[^A-Za-z0-9+/]/g, '');
    if (normalized.length % 4 === 1) normalized = normalized.slice(0, -1);
    if (nativeFromBase64) return nativeFromBase64(normalized);
    const binary = atob(normalized);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; ++i) bytes[i] = binary.charCodeAt(i);
    return bytes;
  }

  function encodeBase64(bytes, urlSafe) {
    if (nativeToBase64) {
      const view = nativeBase64HandlesViews || bytes.byteOffset === 0 ?
          bytes : new Uint8Array(bytes);
      return nativeToBase64.call(view, urlSafe ?
          {alphabet: 'base64url', omitPadding: true} : undefined);
    }
    // Older JS hosts lack the native API. Bounded chunks avoid both a long
    // chain of per-byte strings and the argument limit of one large apply().
    const chunks = [];
    for (let offset = 0; offset < bytes.length; offset += 8192) {
      chunks.push(String.fromCharCode.apply(
          null, bytes.subarray(offset, offset + 8192)));
    }
    const encoded = btoa(chunks.join(''));
    return urlSafe ? encoded.replace(/\+/g, '-').replace(/\//g, '_')
        .replace(/=+$/, '') : encoded;
  }

  function bufferEncodingName(encoding) {
    switch (encoding.toLowerCase()) {
      case 'utf8': case 'utf-8': return 'utf8';
      case 'utf16le': case 'utf-16le': case 'ucs2': case 'ucs-2': return 'utf16le';
      case 'latin1': case 'binary': return 'latin1';
      case 'ascii': case 'base64': case 'base64url': case 'hex':
        return encoding.toLowerCase();
    }
  }

  function resolveBufferEncoding(encoding) {
    // toString/write coerce encodings; from(string) deliberately does not.
    const name = bufferEncodingName(encoding + '');
    if (name !== undefined) return name;
    const error = new TypeError('Unknown encoding: ' + encoding);
    error.code = 'ERR_UNKNOWN_ENCODING';
    throw error;
  }

  function validateBufferOffset(value, name, maximum) {
    if (typeof value !== 'number') {
      const error = new TypeError(name + ' must be a number');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
    if (!Number.isInteger(value) || value < 0 || value > maximum) {
      const error = new RangeError(name + ' is outside the bounds of the Buffer');
      error.code = 'ERR_OUT_OF_RANGE';
      throw error;
    }
  }

  function encodeBufferString(value, encoding) {
    if (encoding === 'utf8') return textEncoder.encode(value);
    if (encoding === 'base64' || encoding === 'base64url') return decodeBase64(value);
    if (encoding === 'hex') {
      const bytes = new Uint8Array(Math.floor(value.length / 2));
      let length = 0;
      for (; length < bytes.length; ++length) {
        const high = bufferHexDigit(value.charCodeAt(length * 2));
        const low = bufferHexDigit(value.charCodeAt(length * 2 + 1));
        if (high < 0 || low < 0) break;
        bytes[length] = high * 16 + low;
      }
      // Incomplete pairs and the first invalid character terminate decoding.
      return bytes.subarray(0, length);
    }
    const wide = encoding === 'utf16le';
    const bytes = new Uint8Array(value.length * (wide ? 2 : 1));
    for (let i = 0; i < value.length; ++i) {
      const code = value.charCodeAt(i);
      bytes[wide ? i * 2 : i] = code;
      if (wide) bytes[i * 2 + 1] = code >>> 8;
    }
    return bytes;
  }

  function bufferHexDigit(code) {
    code &= 0xff;
    if (code >= 48 && code <= 57) return code - 48;
    code |= 32;
    return code >= 97 && code <= 102 ? code - 87 : -1;
  }

  function decodeBufferString(bytes, encoding) {
    if (encoding === 'utf8') return textDecoder.decode(bytes);
    if (encoding === 'base64' || encoding === 'base64url') {
      return encodeBase64(bytes, encoding === 'base64url');
    }
    if (encoding === 'hex') {
      return Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
    }
    const wide = encoding === 'utf16le';
    const length = wide ? Math.floor(bytes.length / 2) : bytes.length;
    const chunks = [];
    for (let start = 0; start < length; start += 8192) {
      const codes = [];
      for (let i = start; i < Math.min(length, start + 8192); ++i) {
        codes.push(wide ? bytes[i * 2] | bytes[i * 2 + 1] << 8 :
            bytes[i] & (encoding === 'ascii' ? 0x7f : 0xff));
      }
      // UTF-16LE must preserve lone surrogates and discard an odd final byte.
      // Latin-1 maps bytes directly (TextDecoder's latin1 means Windows-1252).
      chunks.push(String.fromCharCode.apply(null, codes));
    }
    return chunks.join('');
  }

  class Buffer extends Uint8Array {
    static from(value, encoding, length) {
      if (typeof value === 'string') {
        const name = typeof encoding !== 'string' || encoding === '' ?
            'utf8' : resolveBufferEncoding(encoding);
        const bytes = encodeBufferString(value, name);
        return new Buffer(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      }
      if (ArrayBuffer.isView(value)) {
        // Typed arrays contribute elements, not their underlying byte layout.
        return new Buffer(value);
      }
      let bufferLength;
      try { bufferLength = arrayBufferByteLength.call(value); } catch (_) {}
      if (bufferLength === undefined && sharedArrayBufferByteLength) {
        try { bufferLength = sharedArrayBufferByteLength.call(value); } catch (_) {}
      }
      if (bufferLength !== undefined) {
        let offset = encoding === undefined ? 0 : +encoding;
        if (Number.isNaN(offset)) offset = 0;
        const available = bufferLength - offset;
        if (length !== undefined) {
          length = +length;
          if (!(length > 0)) length = 0;
        }
        if (available < 0 || (length !== undefined && length > available)) {
          const error = new RangeError('Buffer offset or length is outside the ArrayBuffer');
          error.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
          throw error;
        }
        return new Buffer(value, offset, length);
      }
      if (Array.isArray(value)) {
        return new Buffer(Uint8Array.from(value).buffer);
      }
      return new Buffer(0);
    }

    static alloc(size, fill = 0, encoding) {
      const buf = new Buffer(size);
      if (fill !== 0 && size > 0) buf.fill(fill, 0, buf.length, encoding);
      return buf;
    }

    static isBuffer(obj) {
      return obj instanceof Buffer;
    }

    static isEncoding(encoding) {
      // Node accepts primitive strings only; do not coerce application objects.
      return typeof encoding === 'string' && bufferEncodingName(encoding) !== undefined;
    }

    static byteLength(value, encoding) {
      if (typeof value !== 'string') {
        if (ArrayBuffer.isView(value)) return value.byteLength;
        // Intrinsic getters also recognize ArrayBuffers from another realm.
        try { return arrayBufferByteLength.call(value); } catch (_) {}
        if (sharedArrayBufferByteLength) {
          try { return sharedArrayBufferByteLength.call(value); } catch (_) {}
        }
        const error = new TypeError('value must be a string, Buffer, or ArrayBuffer');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      const length = value.length;
      if (length === 0) return 0;
      switch (!encoding ? 'utf8' : bufferEncodingName(encoding + '')) {
        case 'ascii': case 'latin1': return length;
        case 'utf16le':
          return length * 2;
        case 'hex': return Math.floor(length / 2);
        case 'base64': case 'base64url': {
          // Node estimates from the encoded length, including any whitespace.
          let unpadded = length;
          if (unpadded && value.charCodeAt(unpadded - 1) === 61) --unpadded;
          if (unpadded && value.charCodeAt(unpadded - 1) === 61) --unpadded;
          return Math.floor(unpadded * 3 / 4);
        }
      }
      // UTF-8 is also Node's fallback for an unknown encoding. Count directly
      // so Content-Length does not allocate an encoded copy of the request.
      let bytes = 0;
      for (let i = 0; i < length; ++i) {
        const code = value.charCodeAt(i);
        if (code < 0x80) ++bytes;
        else if (code < 0x800) bytes += 2;
        else if (code >= 0xd800 && code <= 0xdbff && i + 1 < length &&
                 value.charCodeAt(i + 1) >= 0xdc00 && value.charCodeAt(i + 1) <= 0xdfff) {
          bytes += 4;
          ++i;
        } else bytes += 3;
      }
      return bytes;
    }

    static concat(list, totalLength) {
      if (!Array.isArray(list)) throw new TypeError('list must be an Array');
      if (list.length === 0) return new Buffer(0);
      if (totalLength === undefined) {
        totalLength = list.reduce((acc, curr) => acc + curr.length, 0);
      }
      const result = new Buffer(totalLength);
      let offset = 0;
      for (const item of list) {
        result.set(item, offset);
        offset += item.length;
      }
      return result;
    }

    static allocUnsafe(size) {
      return new Buffer(size);
    }

    static allocUnsafeSlow(size) {
      return new Buffer(size);
    }

    toString(encoding, start, end) {
      if (start <= 0) start = 0;
      else if (start >= this.length) return '';
      else start = Math.trunc(start) || 0;
      if (end === undefined || end > this.length) end = this.length;
      else end = Math.trunc(end) || 0;
      if (end <= start) return '';
      const name = encoding === undefined ? 'utf8' : resolveBufferEncoding(encoding);
      return decodeBufferString(this.subarray(start, end), name);
    }

    slice(start, end) {
      return this.subarray(start, end);
    }

    copy(target, targetStart = 0, sourceStart = 0, sourceEnd = this.length) {
      const start = targetStart >>> 0;
      const end = Math.min(this.length, sourceEnd >>> 0);
      let j = sourceStart >>> 0;
      for (let i = start; j < end && i < target.length; i++, j++) {
        target[i] = this[j];
      }
      return j - (sourceStart >>> 0);
    }

    write(string, offset, length, encoding) {
      if (offset === undefined) {
        offset = 0;
        length = this.length;
        encoding = 'utf8';
      } else if (length === undefined && typeof offset === 'string') {
        encoding = offset;
        offset = 0;
        length = this.length;
      } else {
        validateBufferOffset(offset, 'offset', this.length);
        if (typeof length === 'string') {
          encoding = length;
          length = this.length - offset;
        } else if (length === undefined) {
          length = this.length - offset;
        } else {
          validateBufferOffset(length, 'length', this.length);
          length = Math.min(length, this.length - offset);
        }
      }
      const name = !encoding ? 'utf8' : resolveBufferEncoding(encoding);
      if (typeof string !== 'string') {
        const error = new TypeError('string must be a string');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      if (name === 'utf8') {
        // encodeInto writes only complete characters, including replacement
        // characters for lone surrogates, when the destination is too short.
        return textEncoder.encodeInto(string, this.subarray(offset, offset + length)).written;
      }
      const data = encodeBufferString(string, name);
      const maximum = Math.min(data.length,
          name === 'utf16le' ? length - length % 2 : length);
      this.set(data.subarray(0, maximum), offset);
      return maximum;
    }

    fill(value, start, end, encoding) {
      let name;
      if (typeof value === 'string') {
        if (start === undefined || typeof start === 'string') {
          encoding = start;
          start = 0;
          end = this.length;
        } else if (typeof end === 'string') {
          encoding = end;
          end = this.length;
        }
        if (encoding == null || encoding === '') name = 'utf8';
        else if (typeof encoding === 'string') name = resolveBufferEncoding(encoding);
        else {
          const error = new TypeError('encoding must be a string');
          error.code = 'ERR_INVALID_ARG_TYPE';
          throw error;
        }
        if (value === '') value = 0;
      }
      if (start === undefined) {
        start = 0;
        end = this.length;
      } else {
        validateBufferOffset(start, 'offset', Number.MAX_SAFE_INTEGER);
        if (end === undefined) end = this.length;
        else validateBufferOffset(end, 'end', this.length);
        if (start >= end) return this;
      }
      if (typeof value === 'string' || ArrayBuffer.isView(value)) {
        const bytes = typeof value === 'string' ? encodeBufferString(value, name) :
            new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        if (bytes.length === 0) {
          const error = new TypeError('value must encode at least one byte');
          error.code = 'ERR_INVALID_ARG_VALUE';
          throw error;
        }
        const count = Math.min(bytes.length, end - start);
        // set snapshots an overlapping input; repeat from the copied prefix.
        this.set(bytes.subarray(0, count), start);
        for (let i = start + count; i < end; ++i) this[i] = this[start + (i - start) % count];
      } else {
        Uint8Array.prototype.fill.call(this, value, start, end);
      }
      return this;
    }
    equals(other) {
      if (!other || other.length !== this.length) return false;
      for (let i = 0; i < this.length; i++) {
        if (this[i] !== other[i]) return false;
      }
      return true;
    }
  }

  installBufferNumericMethods(Buffer);

  // Node exposes its public static methods as enumerable own properties.
  // Compatibility wrappers copy them with for...in or Object.assign; class
  // syntax would otherwise silently remove those methods from the wrapper.
  for (const name of Object.getOwnPropertyNames(Buffer)) {
    const descriptor = Object.getOwnPropertyDescriptor(Buffer, name);
    if (typeof descriptor.value === 'function') {
      Object.defineProperty(Buffer, name, {...descriptor, enumerable: true});
    }
  }

  return Buffer;
}

function createBufferModule(Buffer, limits) {
  const module = {Buffer, Blob: globalThis.Blob, File: globalThis.File,
    ...createBufferTextValidators()};
  const constants = {};
  for (const [name, alias] of [['MAX_LENGTH', 'kMaxLength'],
      ['MAX_STRING_LENGTH', 'kStringMaxLength']]) {
    const value = limits?.[name];
    if (Number.isSafeInteger(value) && value > 0) {
      Object.defineProperty(constants, name, {value, enumerable: true});
      module[alias] = value;
    } else {
      // No architecture guesses or allocation probes: limits belong to the
      // V8 build that actually creates this runtime's strings and byte arrays.
      const get = () => { throw Object.assign(new Error(
        'Native Buffer limits are unavailable'), {code: 'ERR_NOT_SUPPORTED'}); };
      Object.defineProperty(constants, name, {get, enumerable: true});
      Object.defineProperty(module, alias, {get, enumerable: true, configurable: true});
    }
  }
  Object.defineProperty(module, 'constants', {value: constants, enumerable: true});
  return module;
}

// Validate byte sequences without decoding/copying or allocating an output
// string. TypedArray getters preserve the exact byte range across realms and
// cannot be fooled by shadowed buffer/byteLength properties or toStringTag.
function createBufferTextValidators() {
  const typed = Object.getPrototypeOf(Uint8Array.prototype);
  const getter = (prototype, name) => Object.getOwnPropertyDescriptor(prototype, name).get;
  const typedBuffer = getter(typed, 'buffer');
  const typedOffset = getter(typed, 'byteOffset');
  const typedLength = getter(typed, 'byteLength');
  const arrayLength = getter(ArrayBuffer.prototype, 'byteLength');
  const sharedLength = typeof SharedArrayBuffer === 'function' ?
    getter(SharedArrayBuffer.prototype, 'byteLength') : null;
  function bytes(input) {
    let buffer, offset, length;
    try {
      buffer = typedBuffer.call(input);
      offset = typedOffset.call(input);
      length = typedLength.call(input);
      // Node treats a detached/out-of-bounds TypedArray as an empty sequence.
      return length ? new Uint8Array(buffer, offset, length) : new Uint8Array(0);
    } catch (_) {}
    let valid = false;
    try { length = arrayLength.call(input); valid = true; } catch (_) {}
    if (!valid && sharedLength) {
      try { length = sharedLength.call(input); valid = true; } catch (_) {}
    }
    if (!valid) throw Object.assign(new TypeError(
      'input must be an ArrayBuffer, Buffer or TypedArray'), {code:'ERR_INVALID_ARG_TYPE'});
    try { return new Uint8Array(input, 0, length); }
    catch (_) { throw Object.assign(new Error('Cannot validate on a detached buffer'),
      {code:'ERR_INVALID_STATE'}); }
  }
  function isAscii(input) {
    for (const byte of bytes(input)) if (byte > 0x7f) return false;
    return true;
  }
  function isUtf8(input) {
    const data = bytes(input);
    for (let i = 0; i < data.length;) {
      const lead = data[i++];
      if (lead <= 0x7f) continue;
      let remaining, low = 0x80, high = 0xbf;
      if (lead >= 0xc2 && lead <= 0xdf) remaining = 1;
      else if (lead >= 0xe0 && lead <= 0xef) {
        remaining = 2;
        if (lead === 0xe0) low = 0xa0;
        if (lead === 0xed) high = 0x9f;
      } else if (lead >= 0xf0 && lead <= 0xf4) {
        remaining = 3;
        if (lead === 0xf0) low = 0x90;
        if (lead === 0xf4) high = 0x8f;
      } else return false;
      if (i + remaining > data.length || data[i] < low || data[i] > high) return false;
      ++i;
      while (--remaining) if ((data[i++] & 0xc0) !== 0x80) return false;
    }
    return true;
  }
  return {isAscii, isUtf8};
}

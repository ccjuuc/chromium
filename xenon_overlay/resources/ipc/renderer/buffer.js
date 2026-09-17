  // --- 5. Buffer & Util ---
  const textEncoder = new TextEncoder();
  // Buffer treats a UTF-8 BOM as content, unlike TextDecoder's default.
  const textDecoder = new TextDecoder('utf-8', {ignoreBOM: true});
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
      if (value instanceof ArrayBuffer) {
        let offset = encoding === undefined ? 0 : +encoding;
        if (Number.isNaN(offset)) offset = 0;
        const available = value.byteLength - offset;
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

    writeUInt8(value, offset = 0) {
      this[offset] = value & 0xff;
      return offset + 1;
    }
    readUInt8(offset = 0) {
      return this[offset];
    }
    readUIntLE(offset, byteLength) {
      if (offset === undefined || typeof byteLength !== 'number') {
        const error = new TypeError('offset and byteLength must be numbers');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      if (!Number.isInteger(byteLength) || byteLength < 1 || byteLength > 6) {
        const error = new RangeError('byteLength must be an integer from 1 to 6');
        error.code = 'ERR_OUT_OF_RANGE';
        throw error;
      }
      if (typeof offset !== 'number') {
        const error = new TypeError('offset must be a number');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      if (this[offset] === undefined || this[offset + byteLength - 1] === undefined) {
        const error = new RangeError('offset is outside the bounds of the Buffer');
        error.code = Math.floor(offset) === offset && this.length < byteLength ?
            'ERR_BUFFER_OUT_OF_BOUNDS' : 'ERR_OUT_OF_RANGE';
        throw error;
      }
      let value = 0;
      for (let i = byteLength - 1; i >= 0; --i) {
        value = value * 256 + this[offset + i];
      }
      return value;
    }
    writeInt8(value, offset = 0) {
      this[offset] = value & 0xff;
      return offset + 1;
    }
    readInt8(offset = 0) {
      const v = this[offset];
      return v & 0x80 ? v - 0x100 : v;
    }
    writeUInt16LE(value, offset = 0) {
      this[offset] = value & 0xff;
      this[offset + 1] = (value >>> 8) & 0xff;
      return offset + 2;
    }
    readUInt16LE(offset = 0) {
      return this[offset] | (this[offset + 1] << 8);
    }
    readInt16LE(offset = 0) {
      const value = this.readUIntLE(offset, 2);
      return value & 0x8000 ? value - 0x10000 : value;
    }
    writeUInt16BE(value, offset = 0) {
      this[offset] = (value >>> 8) & 0xff;
      this[offset + 1] = value & 0xff;
      return offset + 2;
    }
    readUInt16BE(offset = 0) {
      return (this[offset] << 8) | this[offset + 1];
    }
    writeUInt32LE(value, offset = 0) {
      this[offset] = value & 0xff;
      this[offset + 1] = (value >>> 8) & 0xff;
      this[offset + 2] = (value >>> 16) & 0xff;
      this[offset + 3] = (value >>> 24) & 0xff;
      return offset + 4;
    }
    readUInt32LE(offset = 0) {
      return (
          (this[offset] | (this[offset + 1] << 8) | (this[offset + 2] << 16) |
           (this[offset + 3] << 24)) >>>
          0);
    }
    writeUInt32BE(value, offset = 0) {
      this[offset] = (value >>> 24) & 0xff;
      this[offset + 1] = (value >>> 16) & 0xff;
      this[offset + 2] = (value >>> 8) & 0xff;
      this[offset + 3] = value & 0xff;
      return offset + 4;
    }
    readUInt32BE(offset = 0) {
      return (
          ((this[offset] << 24) | (this[offset + 1] << 16) |
           (this[offset + 2] << 8) | this[offset + 3]) >>>
          0);
    }
    writeInt32LE(value, offset = 0) {
      return this.writeUInt32LE(value, offset);
    }
    readInt32LE(offset = 0) {
      return this.readUInt32LE(offset) | 0;
    }
    writeInt32BE(value, offset = 0) {
      return this.writeUInt32BE(value, offset);
    }
    readInt32BE(offset = 0) {
      return this.readUInt32BE(offset) | 0;
    }
    writeDoubleLE(value, offset = 0) {
      new DataView(this.buffer, this.byteOffset + offset, 8).setFloat64(0, value, true);
      return offset + 8;
    }
    readDoubleLE(offset = 0) {
      return new DataView(this.buffer, this.byteOffset + offset, 8).getFloat64(0, true);
    }
    writeFloatLE(value, offset = 0) {
      new DataView(this.buffer, this.byteOffset + offset, 4).setFloat32(0, value, true);
      return offset + 4;
    }
    readFloatLE(offset = 0) {
      return new DataView(this.buffer, this.byteOffset + offset, 4).getFloat32(0, true);
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

  Object.defineProperty(Buffer.prototype, 'readUintLE', {
    value: Buffer.prototype.readUIntLE, configurable: true, writable: true,
  });

  const utilModule = {
    promisify: fn => (...args) => new Promise((resolve, reject) => {
      fn(...args, (err, res) => err ? reject(err) : resolve(res));
    }),
    callbackify(fn) {
      return (...args) => {
        const cb = args.pop();
        fn(...args).then(res => cb(null, res), err => cb(err));
      };
    },
    format: (...args) => args.map(a => typeof a === 'object' ? JSON.stringify(a) : String(a)).join(' '),
    inspect: obj => {
      try { return JSON.stringify(obj, null, 2); } catch { return String(obj); }
    },
    inherits: (ctor, superCtor) => {
      if (typeof ctor !== 'function' || typeof superCtor !== 'function') {
        throw new TypeError('The constructor and super constructor must be functions');
      }
      ctor.super_ = superCtor;
      Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    },
    types: {
      isDate: (v) => v instanceof Date,
      isRegExp: (v) => v instanceof RegExp,
      isNativeError: (v) => v instanceof Error,
      isBuffer: (v) => Buffer.isBuffer(v),
      isArrayBuffer: (v) => v instanceof ArrayBuffer,
      isUint8Array: (v) => v instanceof Uint8Array,
      isPromise: (v) => !!v && typeof v.then === 'function',
    },
    isArray: Array.isArray,
    isBoolean: (v) => typeof v === 'boolean',
    isBuffer: (v) => Buffer.isBuffer(v),
    isDate: (v) => v instanceof Date,
    isError: (v) => v instanceof Error,
    isFunction: (v) => typeof v === 'function',
    isNull: (v) => v === null,
    isNullOrUndefined: (v) => v == null,
    isNumber: (v) => typeof v === 'number',
    isObject: (v) => typeof v === 'object' && v !== null,
    isPrimitive: (v) => v === null || (typeof v !== 'object' && typeof v !== 'function'),
    isRegExp: (v) => v instanceof RegExp,
    isString: (v) => typeof v === 'string',
    isSymbol: (v) => typeof v === 'symbol',
    isUndefined: (v) => v === undefined,
    deprecate: (fn) => fn,
    TextEncoder,
    TextDecoder,
  };
  utilModule.default = utilModule;

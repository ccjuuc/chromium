  // =========================================================================
  // Node.js Built-in Modules (Buffer, fs, crypto, util, url, stream, etc.)
  // =========================================================================

  const nativeToBase64 = Uint8Array.prototype.toBase64;
  const nativeFromBase64 = Uint8Array.fromBase64;
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
    let normalized = value.split('=', 1)[0].replace(/-/g, '+')
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
    const chunks = [];
    for (let offset = 0; offset < bytes.length; offset += 8192) {
      chunks.push(String.fromCharCode.apply(
          null, bytes.subarray(offset, offset + 8192)));
    }
    const encoded = btoa(chunks.join(''));
    return urlSafe ? encoded.replace(/\+/g, '-').replace(/\//g, '_')
        .replace(/=+$/, '') : encoded;
  }

  class Buffer extends Uint8Array {
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
      switch (typeof encoding === 'string' ? encoding.toLowerCase() : '') {
        case 'ascii': case 'latin1': case 'binary': return length;
        case 'utf16le': case 'utf-16le': case 'ucs2': case 'ucs-2':
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
    static from(data, encoding, length) {
      if (typeof data === 'string') {
        if (encoding === 'hex') {
          const bytes = [];
          for (let i = 0; i < data.length; i += 2) {
            bytes.push(parseInt(data.substr(i, 2), 16));
          }
          return new Buffer(bytes);
        } else if (encoding === 'base64' || encoding === 'base64url') {
          const bytes = decodeBase64(data);
          return new Buffer(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        }
        const encoder = new TextEncoder();
        return new Buffer(encoder.encode(data));
      }
      if (data instanceof ArrayBuffer) {
        let offset = encoding === undefined ? 0 : +encoding;
        if (Number.isNaN(offset)) offset = 0;
        const available = data.byteLength - offset;
        if (length !== undefined) {
          length = +length;
          if (!(length > 0)) length = 0;
        }
        if (available < 0 || (length !== undefined && length > available)) {
          const error = new RangeError('Buffer offset or length is outside the ArrayBuffer');
          error.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
          throw error;
        }
        return new Buffer(data, offset, length);
      }
      if (Array.isArray(data) || ArrayBuffer.isView(data)) {
        return new Buffer(data);
      }
      return new Buffer(0);
    }
    static alloc(size, fill = 0) {
      const b = new Buffer(size);
      if (fill) b.fill(fill);
      return b;
    }
    static allocUnsafe(size) { return new Buffer(size); }
    static isBuffer(obj) { return obj instanceof Buffer; }
    static concat(list, totalLength) {
      if (!Array.isArray(list)) return new Buffer(0);
      if (totalLength === undefined) {
        totalLength = list.reduce((acc, cur) => acc + (cur ? cur.length : 0), 0);
      }
      const result = new Buffer(totalLength);
      let offset = 0;
      for (const item of list) {
        if (item) {
          result.set(item, offset);
          offset += item.length;
        }
      }
      return result;
    }
    toString(encoding = 'utf8', start = 0, end = this.length) {
      const slice = this.subarray(start, end);
      if (encoding === 'hex') {
        return Array.from(slice).map(b => b.toString(16).padStart(2, '0')).join('');
      } else if (encoding === 'base64' || encoding === 'base64url') {
        return encodeBase64(slice, encoding === 'base64url');
      }
      return new TextDecoder().decode(slice);
    }
    slice(start, end) { return this.subarray(start, end); }
    _view() {
      return new DataView(this.buffer, this.byteOffset, this.byteLength);
    }
    writeUInt32LE(value, offset = 0) {
      this._view().setUint32(offset, value >>> 0, true);
      return offset + 4;
    }
    writeBigUInt64LE(value, offset = 0) {
      this._view().setBigUint64(offset, BigInt(value), true);
      return offset + 8;
    }
    readUInt32LE(offset = 0) {
      return this._view().getUint32(offset, true);
    }
    readInt32LE(offset = 0) {
      return this._view().getInt32(offset, true);
    }
    readInt16LE(offset = 0) {
      const value = this.readUIntLE(offset, 2);
      return value & 0x8000 ? value - 0x10000 : value;
    }
    readBigUInt64LE(offset = 0) {
      return this._view().getBigUint64(offset, true);
    }
    readBigInt64LE(offset = 0) {
      return this._view().getBigInt64(offset, true);
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
  }
  Object.defineProperty(Buffer.prototype, 'readUintLE', {
    value: Buffer.prototype.readUIntLE, configurable: true, writable: true,
  });
  globalThis.Buffer = Buffer;

  function streamUnsupportedError(method) {
    const error = new Error(`stream.${method} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    return error;
  }
  function streamValidateCallback(callback) {
    if (typeof callback !== 'function') {
      const error = new TypeError('The callback argument must be a function');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
  }
  function streamFailOperation(method, callback) {
    const error = streamUnsupportedError(method);
    if (callback === undefined) throw error;
    streamValidateCallback(callback);
    queueMicrotask(() => callback(error));
  }

  // Keep the callable constructors and EventEmitter inheritance used by
  // userland stream implementations. These base classes do not implement an
  // I/O queue: operations must fail explicitly until a subclass supplies one.
  function Stream() {
    if (!(this instanceof Stream)) return new Stream();
    EventEmitter.call(this);
  }
  Object.setPrototypeOf(Stream.prototype, EventEmitter.prototype);
  Object.setPrototypeOf(Stream, EventEmitter);
  Stream.prototype.pipe = function() { throw streamUnsupportedError('pipe'); };

  function Readable(options) {
    if (!(this instanceof Readable)) return new Readable(options);
    Stream.call(this);
    if (options) {
      if (typeof options.read === 'function') this._read = options.read;
      if (typeof options.destroy === 'function') this._destroy = options.destroy;
    }
  }
  Object.setPrototypeOf(Readable.prototype, Stream.prototype);
  Object.setPrototypeOf(Readable, Stream);
  Readable.prototype.read = function() { throw streamUnsupportedError('Readable.read'); };
  Readable.prototype.push = function() { throw streamUnsupportedError('Readable.push'); };

  function Writable(options) {
    if (!(this instanceof Writable)) return new Writable(options);
    Stream.call(this);
    if (options && typeof options.write === 'function') this._write = options.write;
  }
  Object.setPrototypeOf(Writable.prototype, Stream.prototype);
  Object.setPrototypeOf(Writable, Stream);
  Writable.prototype.write = function(chunk, encoding, callback) {
    if (typeof encoding === 'function') callback = encoding;
    streamFailOperation('Writable.write', callback);
    return false;
  };
  Writable.prototype.end = function(chunk, encoding, callback) {
    if (typeof chunk === 'function') callback = chunk;
    else if (typeof encoding === 'function') callback = encoding;
    streamFailOperation('Writable.end', callback);
    return this;
  };

  function Duplex(options) {
    if (!(this instanceof Duplex)) return new Duplex(options);
    Readable.call(this, options);
    if (options && typeof options.write === 'function') this._write = options.write;
  }
  Object.setPrototypeOf(Duplex.prototype, Readable.prototype);
  Object.setPrototypeOf(Duplex, Readable);
  Duplex.prototype.write = Writable.prototype.write;
  Duplex.prototype.end = Writable.prototype.end;

  function Transform(options) {
    if (!(this instanceof Transform)) return new Transform(options);
    Duplex.call(this, options);
    if (options && typeof options.transform === 'function') {
      this._transform = options.transform;
    }
  }
  Object.setPrototypeOf(Transform.prototype, Duplex.prototype);
  Object.setPrototypeOf(Transform, Duplex);
  Transform.prototype._transform = function(chunk, encoding, callback) {
    streamFailOperation('Transform._transform', callback);
  };

  function PassThrough(options) {
    if (!(this instanceof PassThrough)) return new PassThrough(options);
    Transform.call(this, options);
  }
  Object.setPrototypeOf(PassThrough.prototype, Transform.prototype);
  Object.setPrototypeOf(PassThrough, Transform);

  Stream.Stream = Stream;
  Stream.Readable = Readable;
  Stream.Writable = Writable;
  Stream.Duplex = Duplex;
  Stream.Transform = Transform;
  Stream.PassThrough = PassThrough;
  Stream.EventEmitter = EventEmitter;
  Stream.pipeline = (...args) => {
    const callback = args.pop();
    streamValidateCallback(callback);
    streamFailOperation('pipeline', callback);
    const streams = Array.isArray(args[0]) ? args[0] : args;
    return streams[streams.length - 1];
  };
  Stream.finished = (stream, options, callback) => {
    if (typeof options === 'function') callback = options;
    streamValidateCallback(callback);
    let active = true;
    queueMicrotask(() => {
      if (active) callback(streamUnsupportedError('finished'));
    });
    return () => { active = false; };
  };
  Stream.default = Stream;
  const streamModule = Stream;

  function rotl32(v, n) {
    return (v << n) | (v >>> (32 - n));
  }

  function md5Bytes(bytes) {
    const originalLen = bytes.length;
    const bitLen = originalLen * 8;
    const paddedLen = (((originalLen + 8) >> 6) + 1) << 6;
    const buf = new Uint8Array(paddedLen);
    buf.set(bytes);
    buf[originalLen] = 0x80;
    const view = new DataView(buf.buffer);
    view.setUint32(paddedLen - 8, bitLen, true);
    view.setUint32(paddedLen - 4, Math.floor(bitLen / 0x100000000), true);

    let a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    const ff = (x, y, z) => (x & y) | (~x & z);
    const gg = (x, y, z) => (x & z) | (y & ~z);
    const hh = (x, y, z) => x ^ y ^ z;
    const ii = (x, y, z) => y ^ (x | ~z);
    const cmn = (q, a0, b0, x, s, t) =>
        (rotl32((a0 + q + x + t) | 0, s) + b0) | 0;

    for (let i = 0; i < paddedLen; i += 64) {
      const x = new Int32Array(16);
      for (let j = 0; j < 16; j++) x[j] = view.getInt32(i + j * 4, true);
      let aa = a, bb = b, cc = c, dd = d;
      const r = (f, k, s, t) => {
        const fval = f(b, c, d);
        const tmp = d;
        d = c;
        c = b;
        b = cmn(fval, a, b, x[k], s, t);
        a = tmp;
      };
      r(ff, 0, 7, 0xd76aa478); r(ff, 1, 12, 0xe8c7b756);
      r(ff, 2, 17, 0x242070db); r(ff, 3, 22, 0xc1bdceee);
      r(ff, 4, 7, 0xf57c0faf); r(ff, 5, 12, 0x4787c62a);
      r(ff, 6, 17, 0xa8304613); r(ff, 7, 22, 0xfd469501);
      r(ff, 8, 7, 0x698098d8); r(ff, 9, 12, 0x8b44f7af);
      r(ff, 10, 17, 0xffff5bb1); r(ff, 11, 22, 0x895cd7be);
      r(ff, 12, 7, 0x6b901122); r(ff, 13, 12, 0xfd987193);
      r(ff, 14, 17, 0xa679438e); r(ff, 15, 22, 0x49b40821);
      r(gg, 1, 5, 0xf61e2562); r(gg, 6, 9, 0xc040b340);
      r(gg, 11, 14, 0x265e5a51); r(gg, 0, 20, 0xe9b6c7aa);
      r(gg, 5, 5, 0xd62f105d); r(gg, 10, 9, 0x02441453);
      r(gg, 15, 14, 0xd8a1e681); r(gg, 4, 20, 0xe7d3fbc8);
      r(gg, 9, 5, 0x21e1cde6); r(gg, 14, 9, 0xc33707d6);
      r(gg, 3, 14, 0xf4d50d87); r(gg, 8, 20, 0x455a14ed);
      r(gg, 13, 5, 0xa9e3e905); r(gg, 2, 9, 0xfcefa3f8);
      r(gg, 7, 14, 0x676f02d9); r(gg, 12, 20, 0x8d2a4c8a);
      r(hh, 5, 4, 0xfffa3942); r(hh, 8, 11, 0x8771f681);
      r(hh, 11, 16, 0x6d9d6122); r(hh, 14, 23, 0xfde5380c);
      r(hh, 1, 4, 0xa4beea44); r(hh, 4, 11, 0x4bdecfa9);
      r(hh, 7, 16, 0xf6bb4b60); r(hh, 10, 23, 0xbebfbc70);
      r(hh, 13, 4, 0x289b7ec6); r(hh, 0, 11, 0xeaa127fa);
      r(hh, 3, 16, 0xd4ef3085); r(hh, 6, 23, 0x04881d05);
      r(hh, 9, 4, 0xd9d4d039); r(hh, 12, 11, 0xe6db99e5);
      r(hh, 15, 16, 0x1fa27cf8); r(hh, 2, 23, 0xc4ac5665);
      r(ii, 0, 6, 0xf4292244); r(ii, 7, 10, 0x432aff97);
      r(ii, 14, 15, 0xab9423a7); r(ii, 5, 21, 0xfc93a039);
      r(ii, 12, 6, 0x655b59c3); r(ii, 3, 10, 0x8f0ccc92);
      r(ii, 10, 15, 0xffeff47d); r(ii, 1, 21, 0x85845dd1);
      r(ii, 8, 6, 0x6fa87e4f); r(ii, 15, 10, 0xfe2ce6e0);
      r(ii, 6, 15, 0xa3014314); r(ii, 13, 21, 0x4e0811a1);
      r(ii, 4, 6, 0xf7537e82); r(ii, 11, 10, 0xbd3af235);
      r(ii, 2, 15, 0x2ad7d2bb); r(ii, 9, 21, 0xeb86d391);
      a = (a + aa) | 0; b = (b + bb) | 0; c = (c + cc) | 0; d = (d + dd) | 0;
    }

    const out = new Uint8Array(16);
    const outView = new DataView(out.buffer);
    outView.setInt32(0, a, true);
    outView.setInt32(4, b, true);
    outView.setInt32(8, c, true);
    outView.setInt32(12, d, true);
    return out;
  }

  function sha1Bytes(bytes) {
    const originalLen = bytes.length;
    const bitLenHi = Math.floor(originalLen / 0x20000000);
    const bitLenLo = (originalLen << 3) >>> 0;
    const paddedLen = (((originalLen + 8) >> 6) + 1) << 6;
    const buf = new Uint8Array(paddedLen);
    buf.set(bytes);
    buf[originalLen] = 0x80;
    const view = new DataView(buf.buffer);
    view.setUint32(paddedLen - 8, bitLenHi);
    view.setUint32(paddedLen - 4, bitLenLo);

    let h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe;
    let h3 = 0x10325476, h4 = 0xc3d2e1f0;
    const w = new Uint32Array(80);
    for (let i = 0; i < paddedLen; i += 64) {
      for (let j = 0; j < 16; j++) w[j] = view.getUint32(i + j * 4);
      for (let j = 16; j < 80; j++) {
        w[j] = rotl32(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1) >>> 0;
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4;
      for (let j = 0; j < 80; j++) {
        let f, k;
        if (j < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (j < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        const temp = (rotl32(a, 5) + f + e + k + w[j]) >>> 0;
        e = d; d = c; c = rotl32(b, 30) >>> 0; b = a; a = temp;
      }
      h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0;
      h3 = (h3 + d) >>> 0; h4 = (h4 + e) >>> 0;
    }
    const out = new Uint8Array(20);
    const outView = new DataView(out.buffer);
    outView.setUint32(0, h0); outView.setUint32(4, h1); outView.setUint32(8, h2);
    outView.setUint32(12, h3); outView.setUint32(16, h4);
    return out;
  }

  const kSha256K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ];

  function sha256Bytes(bytes) {
    const originalLen = bytes.length;
    const bitLenHi = Math.floor(originalLen / 0x20000000);
    const bitLenLo = (originalLen << 3) >>> 0;
    const paddedLen = (((originalLen + 8) >> 6) + 1) << 6;
    const buf = new Uint8Array(paddedLen);
    buf.set(bytes);
    buf[originalLen] = 0x80;
    const view = new DataView(buf.buffer);
    view.setUint32(paddedLen - 8, bitLenHi);
    view.setUint32(paddedLen - 4, bitLenLo);

    let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    const w = new Uint32Array(64);
    const rotr = (x, n) => (x >>> n) | (x << (32 - n));
    for (let i = 0; i < paddedLen; i += 64) {
      for (let j = 0; j < 16; j++) w[j] = view.getUint32(i + j * 4);
      for (let j = 16; j < 64; j++) {
        const s0 = rotr(w[j - 15], 7) ^ rotr(w[j - 15], 18) ^ (w[j - 15] >>> 3);
        const s1 = rotr(w[j - 2], 17) ^ rotr(w[j - 2], 19) ^ (w[j - 2] >>> 10);
        w[j] = (w[j - 16] + s0 + w[j - 7] + s1) >>> 0;
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, hh = h7;
      for (let j = 0; j < 64; j++) {
        const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const ch = (e & f) ^ (~e & g);
        const temp1 = (hh + S1 + ch + kSha256K[j] + w[j]) >>> 0;
        const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const temp2 = (S0 + maj) >>> 0;
        hh = g; g = f; f = e; e = (d + temp1) >>> 0;
        d = c; c = b; b = a; a = (temp1 + temp2) >>> 0;
      }
      h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0;
      h3 = (h3 + d) >>> 0; h4 = (h4 + e) >>> 0; h5 = (h5 + f) >>> 0;
      h6 = (h6 + g) >>> 0; h7 = (h7 + hh) >>> 0;
    }
    const out = new Uint8Array(32);
    const outView = new DataView(out.buffer);
    outView.setUint32(0, h0); outView.setUint32(4, h1);
    outView.setUint32(8, h2); outView.setUint32(12, h3);
    outView.setUint32(16, h4); outView.setUint32(20, h5);
    outView.setUint32(24, h6); outView.setUint32(28, h7);
    return out;
  }

  function hashBytes(algorithm, bytes) {
    const algo = String(algorithm || '').toLowerCase().replace(/-/g, '');
    if (algo === 'md5') return md5Bytes(bytes);
    if (algo === 'sha1') return sha1Bytes(bytes);
    if (algo === 'sha256') return sha256Bytes(bytes);
    throw new Error('Unsupported digest algorithm: ' + algorithm);
  }

  function hashBlockSize(algorithm) {
    return 64;
  }

  function snapshotDigestInput(data, encoding) {
    if (typeof data === 'string') return Buffer.from(data, encoding || 'utf8');
    if (ArrayBuffer.isView(data)) {
      return Buffer.from(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
    }
    // Buffer.from(ArrayBuffer) shares its storage; digest inputs must be
    // captured when update/createHmac is called, before user mutation.
    if (Object.prototype.toString.call(data) === '[object ArrayBuffer]' ||
        Object.prototype.toString.call(data) === '[object SharedArrayBuffer]') {
      return Buffer.from(new Uint8Array(data));
    }
    return Buffer.from(data);
  }

  function createHashObject(algorithm, initialChunks) {
    const chunks = initialChunks ? initialChunks.slice() : [];
    return {
      update(data, encoding) {
        chunks.push(snapshotDigestInput(data, encoding));
        return this;
      },
      digest(encoding) {
        const digestBytes = hashBytes(algorithm, Buffer.concat(chunks));
        const buf = Buffer.from(digestBytes);
        return encoding ? buf.toString(encoding) : buf;
      },
    };
  }

  function hmacBytes(algorithm, key, data) {
    const blockSize = hashBlockSize(algorithm);
    let keyBytes = Buffer.from(key);
    if (keyBytes.length > blockSize) {
      keyBytes = Buffer.from(hashBytes(algorithm, keyBytes));
    }
    const ikey = Buffer.alloc(blockSize);
    const okey = Buffer.alloc(blockSize);
    ikey.set(keyBytes);
    okey.set(keyBytes);
    for (let i = 0; i < blockSize; i++) {
      ikey[i] ^= 0x36;
      okey[i] ^= 0x5c;
    }
    const inner = hashBytes(algorithm, Buffer.concat([ikey, data]));
    return hashBytes(algorithm, Buffer.concat([okey, Buffer.from(inner)]));
  }

  function createCipherObject(algorithm, key, iv, encrypt) {
    const normalizedAlgorithm = String(algorithm || '').toLowerCase();
    const keyBuffer = Buffer.isBuffer(key) ? Buffer.from(key) : Buffer.from(key);
    const ivBuffer = iv == null ? Buffer.alloc(0) : Buffer.from(iv);
    const chunks = [];
    let autoPadding = true;
    let finalized = false;
    return {
      update(data, inputEncoding, outputEncoding) {
        if (finalized) {
          throw new Error('Trying to add data in unsupported state');
        }
        chunks.push(typeof data === 'string' ?
            Buffer.from(data, inputEncoding || 'utf8') : Buffer.from(data));
        return outputEncoding ? '' : Buffer.alloc(0);
      },
      final(outputEncoding) {
        if (finalized) {
          throw new Error('Invalid state');
        }
        finalized = true;
        const encoded = transport.sendSync('__xenon:crypto', {
          operation: 'cipher',
          algorithm: normalizedAlgorithm,
          encrypt: Boolean(encrypt),
          keyBase64: keyBuffer.toString('base64'),
          ivBase64: ivBuffer.toString('base64'),
          dataBase64: Buffer.concat(chunks).toString('base64'),
          autoPadding,
        });
        const result = Buffer.from(encoded, 'base64');
        return outputEncoding ? result.toString(outputEncoding) : result;
      },
      setAutoPadding(value = true) {
        if (finalized) {
          throw new Error('Invalid state');
        }
        autoPadding = Boolean(value);
        return this;
      },
    };
  }

  function randomArgumentError(name, value, range) {
    const error = typeof value === 'number' ?
        new RangeError(`${name} is outside ${range}`) :
        new TypeError(`${name} must be a number`);
    error.code = typeof value === 'number' ? 'ERR_OUT_OF_RANGE' : 'ERR_INVALID_ARG_TYPE';
    return error;
  }
  function randomByteCount(value, name, elementSize, maximum) {
    const bytes = typeof value === 'number' ? value * elementSize : NaN;
    if (typeof value !== 'number' || !Number.isFinite(bytes) || bytes < 0 ||
        bytes > Math.min(maximum, 0x7fffffff)) {
      throw randomArgumentError(name, value, `0..${Math.min(maximum, 0x7fffffff)}`);
    }
    return Math.floor(bytes);
  }
  function randomTargetBytes(buf, offset = 0, size) {
    const view = ArrayBuffer.isView(buf);
    const buffer = view ? buf.buffer : buf;
    if (!view && !(buffer instanceof ArrayBuffer) &&
        !(typeof SharedArrayBuffer === 'function' && buffer instanceof SharedArrayBuffer)) {
      const error = new TypeError('buf must be an ArrayBuffer or an ArrayBuffer view');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
    const elementSize = buf.BYTES_PER_ELEMENT || 1;
    const byteOffset = randomByteCount(offset, 'offset', elementSize, buf.byteLength);
    const byteLength = size === undefined ? buf.byteLength - byteOffset :
        randomByteCount(size, 'size', elementSize, buf.byteLength - byteOffset);
    return new Uint8Array(buffer, (view ? buf.byteOffset : 0) + byteOffset, byteLength);
  }
  function fillSecureRandom(bytes) {
    const source = globalThis.crypto;
    if (!source || typeof source.getRandomValues !== 'function') {
      const error = new Error('A secure random source is not available');
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    }
    // WebCrypto limits each call to 65536 bytes, independent of view type.
    for (let offset = 0; offset < bytes.length; offset += 65536) {
      source.getRandomValues(bytes.subarray(offset, offset + 65536));
    }
  }
  function randomCompleteAsync(bytes, buf, callback) {
    queueMicrotask(() => {
      try {
        fillSecureRandom(bytes);
      } catch (error) {
        callback(error);
        return;
      }
      callback(null, buf);
    });
  }

  const cryptoModule = {
    randomBytes(size, callback) {
      if (callback !== undefined) fsValidateCallback(callback);
      const length = randomByteCount(size, 'size', 1, 0x7fffffff);
      const buf = Buffer.alloc(length);
      if (callback !== undefined) {
        randomCompleteAsync(buf, buf, callback);
        return;
      }
      fillSecureRandom(buf);
      return buf;
    },
    randomFillSync(buf, offset = 0, size) {
      fillSecureRandom(randomTargetBytes(buf, offset, size));
      return buf;
    },
    randomFill(buf, offset, size, callback) {
      if (typeof offset === 'function') {
        callback = offset; offset = 0; size = undefined;
      } else if (typeof size === 'function') {
        callback = size; size = undefined;
      }
      fsValidateCallback(callback);
      randomCompleteAsync(randomTargetBytes(buf, offset, size), buf, callback);
    },
    createCipheriv: (algorithm, key, iv) =>
      createCipherObject(algorithm, key, iv, true),
    createDecipheriv: (algorithm, key, iv) =>
      createCipherObject(algorithm, key, iv, false),
    createHash: (algorithm) => createHashObject(algorithm),
    createHmac: (algorithm, key) => {
      const keySnapshot = snapshotDigestInput(key);
      const chunks = [];
      return {
        update(data, encoding) {
          chunks.push(snapshotDigestInput(data, encoding));
          return this;
        },
        digest(encoding) {
          const digestBytes = hmacBytes(
              algorithm, keySnapshot, Buffer.concat(chunks));
          const buf = Buffer.from(digestBytes);
          return encoding ? buf.toString(encoding) : buf;
        },
      };
    },
  };
  cryptoModule.default = cryptoModule;

  function urlError(code, message) {
    return Object.assign(new TypeError(message), {code});
  }

  function fileUrlWindows(options) {
    if (options && options.windows !== undefined && options.windows !== null) {
      if (typeof options.windows !== 'boolean') {
        throw urlError('ERR_INVALID_ARG_TYPE', 'options.windows must be a boolean');
      }
      return options.windows;
    }
    return runtimePlatform === 'win32';
  }

  // WHATWG URL validates and ASCII-encodes hostnames. Node fileURLToPath
  // decodes IDN labels back to Unicode when forming a Windows UNC path.
  function fileUrlUnicodeHost(hostname) {
    return hostname.split('.').map(label => {
      if (!label.startsWith('xn--')) return label;
      const input = label.slice(4);
      const delimiter = input.lastIndexOf('-');
      const output = delimiter < 0 ? [] : Array.from(input.slice(0, delimiter), c => c.codePointAt(0));
      let cursor = delimiter < 0 ? 0 : delimiter + 1;
      let n = 128, index = 0, bias = 72;
      while (cursor < input.length) {
        const oldIndex = index;
        let weight = 1;
        for (let k = 36;; k += 36) {
          const code = input.charCodeAt(cursor++);
          const digit = code >= 48 && code <= 57 ? code - 22 :
              code >= 65 && code <= 90 ? code - 65 : code - 97;
          index += digit * weight;
          const threshold = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
          if (digit < threshold) break;
          weight *= 36 - threshold;
        }
        const size = output.length + 1;
        let delta = Math.floor((index - oldIndex) / (oldIndex === 0 ? 700 : 2));
        delta += Math.floor(delta / size);
        let k = 0;
        while (delta > 455) { delta = Math.floor(delta / 35); k += 36; }
        bias = k + Math.floor(36 * delta / (delta + 38));
        n += Math.floor(index / size);
        index %= size;
        output.splice(index++, 0, n);
      }
      return String.fromCodePoint(...output);
    }).join('.');
  }

  const slashedUrlProtocol = /^(?:https?|ftp|gopher|file|wss?):$/;
  function escapeLegacyUrl(value) {
    return value.replace(/[<>"` \r\n\t{}|\\^']/g, character =>
        '%' + character.charCodeAt(0).toString(16).toUpperCase().padStart(2, '0'));
  }
  function queryStringObject(query) {
    const result = Object.create(null);
    for (const [key, value] of new URLSearchParams(query)) {
      if (!(key in result)) result[key] = value;
      else if (Array.isArray(result[key])) result[key].push(value);
      else result[key] = [result[key], value];
    }
    return result;
  }
  function formatUrlQuery(query) {
    return Object.entries(query || {}).flatMap(([key, values]) =>
      (Array.isArray(values) ? values : [values]).map(value =>
        encodeURIComponent(key) + '=' + encodeURIComponent(
            typeof value === 'string' || typeof value === 'boolean' ||
            (typeof value === 'number' && Number.isFinite(value)) ? value : ''))).join('&');
  }
  class LegacyUrl {
    constructor() {
      for (const key of ['protocol', 'slashes', 'auth', 'host', 'port', 'hostname',
          'hash', 'search', 'query', 'pathname', 'path', 'href']) this[key] = null;
    }
    parse(input, parseQueryString = false, slashesDenoteHost = false) {
      validatePath(input, 'url');
      let rest = input.trim();
      // Legacy URL syntax treats backslashes as slashes only before the query
      // and fragment. Its relative paths do not acquire an origin or host.
      const ending = rest.search(/[?#]/);
      rest = ending < 0 ? rest.replace(/\\/g, '/') :
          rest.slice(0, ending).replace(/\\/g, '/') + rest.slice(ending);
      const protocol = /^([a-z][a-z0-9+.-]*:)/i.exec(rest);
      if (protocol) {
        this.protocol = protocol[1].toLowerCase();
        rest = rest.slice(protocol[0].length);
      }
      const hostless = this.protocol === 'javascript:';
      if (!hostless && rest.startsWith('//') &&
          (this.protocol || slashesDenoteHost || /^\/\/[^/]*@[^/]+/.test(rest))) {
        this.slashes = true;
        rest = rest.slice(2);
      }
      if (!hostless && (this.slashes || (this.protocol && !slashedUrlProtocol.test(this.protocol)))) {
        const hostEnd = rest.search(/[/?#]/);
        let authority = hostEnd < 0 ? rest : rest.slice(0, hostEnd);
        rest = hostEnd < 0 ? '' : rest.slice(hostEnd);
        const at = authority.lastIndexOf('@');
        if (at >= 0) {
          this.auth = decodeURIComponent(authority.slice(0, at));
          authority = authority.slice(at + 1);
        }
        const port = /:([0-9]*)$/.exec(authority);
        if (port) {
          if (port[1]) this.port = port[1];
          authority = authority.slice(0, -port[0].length);
        }
        this.hostname = authority.toLowerCase();
        if (this.hostname.startsWith('[')) {
          if (!this.hostname.endsWith(']')) throw urlError('ERR_INVALID_URL', 'Invalid URL');
          this.hostname = this.hostname.slice(1, -1);
          this.host = '[' + this.hostname + ']';
        } else {
          if (this.hostname && /[^\x00-\x7f]/.test(this.hostname)) {
            this.hostname = new URL('http://' + this.hostname).hostname;
          }
          this.host = this.hostname;
        }
        if (this.port) this.host += ':' + this.port;
      }
      if (!hostless) rest = escapeLegacyUrl(rest);
      const hash = rest.indexOf('#');
      if (hash >= 0) { this.hash = rest.slice(hash); rest = rest.slice(0, hash); }
      const query = rest.indexOf('?');
      if (query >= 0) {
        this.search = rest.slice(query);
        this.query = rest.slice(query + 1);
        rest = rest.slice(0, query);
      }
      if (parseQueryString) {
        this.query = queryStringObject(this.query || '');
      }
      this.pathname = rest || (slashedUrlProtocol.test(this.protocol) && this.hostname ? '/' : null);
      this.path = this.pathname !== null || this.search !== null ?
          (this.pathname || '') + (this.search || '') : null;
      this.href = this.format();
      return this;
    }
    format() {
      let protocol = this.protocol || '';
      if (protocol && !protocol.endsWith(':')) protocol += ':';
      let host = this.host;
      if (!host && this.hostname) {
        host = this.hostname.includes(':') ? '[' + this.hostname + ']' : this.hostname;
        if (this.port) host += ':' + this.port;
      }
      if (this.auth && host !== null && host !== undefined) {
        host = encodeURIComponent(this.auth).replace(/%3A/gi, ':') + '@' + host;
      }
      let pathname = (this.pathname || '').replace(/[?#]/g, char => char === '?' ? '%3F' : '%23');
      if (this.slashes || ((!protocol || slashedUrlProtocol.test(protocol)) && host != null)) {
        host = '//' + (host || '');
        if (pathname && !pathname.startsWith('/')) pathname = '/' + pathname;
      } else host ||= '';
      let search = this.search || (this.query && typeof this.query === 'object' ?
          formatUrlQuery(this.query) : '');
      if (search && !search.startsWith('?')) search = '?' + search;
      let hash = this.hash || '';
      if (hash && !hash.startsWith('#')) hash = '#' + hash;
      return protocol + host + pathname + search.replace(/#/g, '%23') + hash;
    }
    resolve(to) { return this.resolveObject(to).format(); }
    resolveObject(to) {
      const target = typeof to === 'string' ? new LegacyUrl().parse(to, false, true) : to;
      if (target.protocol && target.protocol !== this.protocol) return target;
      const result = Object.assign(new LegacyUrl(), this);
      result.hash = target.hash;
      if (target.slashes || target.host !== null) {
        for (const key of ['slashes', 'auth', 'host', 'hostname', 'port', 'pathname', 'search', 'query']) {
          result[key] = target[key];
        }
      } else if (target.pathname) {
        let pathname = target.pathname.startsWith('/') ? target.pathname :
            (result.pathname || '').replace(/[^/]*$/, '') + target.pathname;
        if (result.host !== null && !pathname.startsWith('/')) pathname = '/' + pathname;
        const trailing = /(?:\/|\/\.{1,2})$/.test(pathname);
        result.pathname = posix.normalize(pathname);
        if (result.pathname === '.') result.pathname = '';
        if (trailing && result.pathname && !result.pathname.endsWith('/')) result.pathname += '/';
        result.search = target.search;
        result.query = target.query;
      } else if (target.search !== null) {
        result.search = target.search;
        result.query = target.query;
      }
      result.path = (result.pathname || '') + (result.search || '') || null;
      result.href = result.format();
      return result;
    }
  }

  const urlModule = {
    Url: LegacyUrl,
    parse: (input, parseQueryString, slashesDenoteHost) => input instanceof LegacyUrl ? input :
        new LegacyUrl().parse(input, parseQueryString, slashesDenoteHost),
    format: input => {
      if (input instanceof URL) return input.href;
      if (typeof input === 'string') return new LegacyUrl().parse(input).format();
      if (!input || typeof input !== 'object') throw urlError('ERR_INVALID_ARG_TYPE', 'url must be an object or string');
      return LegacyUrl.prototype.format.call(input);
    },
    resolve: (from, to) => new LegacyUrl().parse(from, false, true).resolve(to),
    resolveObject: (from, to) => new LegacyUrl().parse(from, false, true).resolveObject(to),
    fileURLToPath: (input, options) => {
      if (typeof input !== 'string' && !(input instanceof URL)) {
        throw urlError('ERR_INVALID_ARG_TYPE', 'path must be a string or URL');
      }
      let url;
      try { url = typeof input === 'string' ? new URL(input) : input; }
      catch (_error) { throw urlError('ERR_INVALID_URL', 'Invalid URL'); }
      if (url.protocol !== 'file:') throw urlError('ERR_INVALID_URL_SCHEME', 'URL must be of scheme file');
      const windows = fileUrlWindows(options);
      if ((windows ? /%(?:2f|5c)/i : /%2f/i).test(url.pathname)) {
        throw urlError('ERR_INVALID_FILE_URL_PATH', 'File URL must not include encoded path separators');
      }
      let pathname = decodeURIComponent(url.pathname);
      if (!windows) {
        if (url.hostname) throw urlError('ERR_INVALID_FILE_URL_HOST', 'File URL host must be empty or localhost');
        return pathname;
      }
      pathname = pathname.replace(/\//g, '\\');
      if (url.hostname) return '\\\\' + fileUrlUnicodeHost(url.hostname) + pathname;
      if (!/^\\[a-zA-Z]:/.test(pathname)) {
        throw urlError('ERR_INVALID_FILE_URL_PATH', 'File URL path must be absolute');
      }
      return pathname.slice(1);
    },
    pathToFileURL: (path, options) => {
      validatePath(path);
      const windows = fileUrlWindows(options);
      const implementation = windows ? win32 : posix;
      let resolved = windows && path.startsWith('\\\\') ? path : implementation.resolve(path);
      if (path.endsWith('/') || (windows && path.endsWith('\\'))) {
        if (!resolved.endsWith(implementation.sep)) resolved += implementation.sep;
      }
      let url = new URL('file:///');
      if (windows && resolved.startsWith('\\\\?\\UNC\\')) {
        resolved = '\\\\' + resolved.slice(8);
      } else if (windows && /^\\\\\?\\[a-zA-Z]:\\/.test(resolved)) {
        resolved = resolved.slice(4);
      }
      if (windows && resolved.startsWith('\\\\')) {
        const serverEnd = resolved.indexOf('\\', 2);
        if (serverEnd < 0 || serverEnd === 2) {
          throw urlError('ERR_INVALID_ARG_VALUE', 'Missing UNC server or resource path');
        }
        const host = resolved.slice(2, serverEnd);
        try { url = new URL('file://' + host + '/'); }
        catch (_error) { throw urlError('ERR_INVALID_URL', 'Invalid UNC server'); }
        resolved = resolved.slice(serverEnd);
      }
      if (windows) resolved = resolved.replace(/\\/g, '/');
      // Assign encoded pathname separately: # and ? are filename characters,
      // never a URL fragment/query, and a literal % must survive decoding.
      url.pathname = resolved.replace(/[%#?\\\x00-\x20]/g, character =>
          '%' + character.charCodeAt(0).toString(16).toUpperCase().padStart(2, '0'));
      return url;
    },
    URL: globalThis.URL,
    URLSearchParams: globalThis.URLSearchParams,
  };
  urlModule.default = urlModule;

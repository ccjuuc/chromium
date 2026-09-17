  const utilModule = {
    TextEncoder: typeof TextEncoder !== 'undefined' ? TextEncoder : class TextEncoder {
      encode(str) {
        str = String(str || '');
        const utf8 = unescape(encodeURIComponent(str));
        const arr = new Uint8Array(utf8.length);
        for (let i = 0; i < utf8.length; i++) arr[i] = utf8.charCodeAt(i);
        return arr;
      }
    },
    TextDecoder: typeof TextDecoder !== 'undefined' ? TextDecoder : class TextDecoder {
      constructor(encoding = 'utf-8') { this.encoding = encoding; }
      decode(arr) {
        if (!arr) return '';
        let str = '';
        const bytes = arr instanceof Uint8Array ? arr : new Uint8Array(arr);
        for (let i = 0; i < bytes.length; i++) str += String.fromCharCode(bytes[i]);
        try { return decodeURIComponent(escape(str)); } catch { return str; }
      }
    },
    promisify(fn) {
      return (...args) => new Promise((resolve, reject) => {
        fn(...args, (err, res) => {
          if (err) return reject(err);
          resolve(res);
        });
      });
    },
    callbackify(fn) {
      return (...args) => {
        const cb = args.pop();
        fn(...args).then(res => cb(null, res), err => cb(err));
      };
    },
    inherits(ctor, superCtor) {
      if (typeof ctor !== 'function' || typeof superCtor !== 'function') {
        throw new TypeError('The constructor and super constructor must be functions');
      }
      ctor.super_ = superCtor;
      Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    },
    format(fmt, ...args) {
      if (typeof fmt !== 'string') return [fmt, ...args].map(String).join(' ');
      let i = 0;
      return fmt.replace(/%[sdjifoO%]/g, m => {
        if (m === '%%') return '%';
        if (i >= args.length) return m;
        const arg = args[i++];
        if (m === '%j') return JSON.stringify(arg);
        return String(arg);
      });
    },
    inspect(obj) {
      try { return JSON.stringify(obj, null, 2); } catch { return String(obj); }
    },
    deprecate(fn, msg) { return fn; },
    isArray: Array.isArray,
    isBoolean: (v) => typeof v === 'boolean',
    isBuffer: (v) => typeof Buffer !== 'undefined' && Buffer.isBuffer && Buffer.isBuffer(v),
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
    types: {
      isPromise: v => v && typeof v.then === 'function',
      isDate: v => v instanceof Date,
      isRegExp: v => v instanceof RegExp,
      isNativeError: v => v instanceof Error,
      isBuffer: v => typeof Buffer !== 'undefined' && Buffer.isBuffer && Buffer.isBuffer(v),
      isArrayBuffer: v => v instanceof ArrayBuffer,
      isUint8Array: v => v instanceof Uint8Array,
    },
  };

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
        const result = Buffer.from(__xenonCryptoCipher(
            normalizedAlgorithm, Boolean(encrypt), keyBuffer.toString('base64'),
            ivBuffer.toString('base64'),
            Buffer.concat(chunks).toString('base64'), autoPadding), 'base64');
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

  function cryptoArgumentError(message) {
    return Object.assign(new TypeError(message), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  function digestEncoding(encoding) {
    if (typeof encoding !== 'string') throw cryptoArgumentError('Encoding must be a string');
    const normalized = encoding.toLowerCase();
    if (normalized === 'utf-8') return 'utf8';
    if (['utf8', 'hex', 'base64', 'base64url'].includes(normalized)) return normalized;
    throw Object.assign(new Error('Unsupported digest encoding: ' + encoding),
                        {code: 'ERR_NOT_SUPPORTED'});
  }
  function cryptoBytes(value, encoding) {
    if (typeof value === 'string') {
      const normalized = digestEncoding(encoding === undefined ? 'utf8' : encoding);
      // Match Node's partial hex decoding, including an unmatched final nibble.
      if (normalized === 'hex') value = value.match(/^(?:[0-9a-fA-F]{2})*/)[0];
      return Buffer.from(value, normalized);
    }
    if (ArrayBuffer.isView(value)) {
      return Buffer.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
    }
    if (value instanceof ArrayBuffer) return Buffer.from(new Uint8Array(value));
    throw cryptoArgumentError('Crypto data must be a string or binary buffer');
  }
  function createDigestObject(algorithm, key) {
    if (typeof algorithm !== 'string') throw cryptoArgumentError('Algorithm must be a string');
    const normalized = algorithm.toLowerCase().replace(/-/g, '');
    if (!['md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512'].includes(normalized)) {
      throw Object.assign(new Error('Unsupported digest algorithm: ' + algorithm),
                          {code: 'ERR_NOT_SUPPORTED'});
    }
    const keyBytes = key === undefined ? null : cryptoBytes(key);
    let chunks = [];
    let finalized = false;
    function checkState() {
      if (finalized) throw Object.assign(new Error('Digest already called'), {code: 'ERR_CRYPTO_HASH_FINALIZED'});
    }
    return {
      update(data, encoding) {
        checkState();
        chunks.push(cryptoBytes(data, encoding));
        return this;
      },
      digest(encoding) {
        checkState();
        const outputEncoding = encoding === undefined ? undefined : digestEncoding(encoding);
        finalized = true;
        const input = Buffer.concat(chunks);
        chunks = [];
        const result = Buffer.from(__xenonCryptoDigest(normalized, input, keyBytes));
        return outputEncoding ? result.toString(outputEncoding) : result;
      },
    };
  }
  const cryptoModule = {
    createCipheriv(algorithm, key, iv) {
      return createCipherObject(algorithm, key, iv, true);
    },
    createDecipheriv(algorithm, key, iv) {
      return createCipherObject(algorithm, key, iv, false);
    },
    createHash(algorithm) {
      return createDigestObject(algorithm);
    },
    createHmac(algorithm, key) {
      if (key === undefined) throw cryptoArgumentError('HMAC requires a key');
      return createDigestObject(algorithm, key);
    },
    randomBytes(size, callback) {
      if (typeof size !== 'number') throw cryptoArgumentError('Size must be a number');
      if (!Number.isInteger(size) || size < 0 || size > 0x7fffffff) {
        throw Object.assign(new RangeError('Size is out of range'), {code: 'ERR_OUT_OF_RANGE'});
      }
      if (callback !== undefined && typeof callback !== 'function') {
        throw cryptoArgumentError('Callback must be a function');
      }
      const generate = () => Buffer.from(__xenonCryptoRandom(size));
      if (callback === undefined) return generate();
      queueMicrotask(() => {
        let bytes;
        try { bytes = generate(); } catch (error) { callback(error); return; }
        callback(null, bytes);
      });
    },
    randomUUID() {
      const bytes = cryptoModule.randomBytes(16);
      bytes[6] = (bytes[6] & 15) | 64;
      bytes[8] = (bytes[8] & 63) | 128;
      const hex = bytes.toString('hex');
      return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
    },
  };

  // Reserved transport used by sandboxed renderers to expose the same
  // synchronous Node cipher contract as the utility main context.
  ipcMain.on('__xenon:crypto', (event, request) => {
    if (!request || request.operation !== 'cipher') {
      throw new TypeError('Invalid crypto request');
    }
    event.returnValue = __xenonCryptoCipher(
        String(request.algorithm || '').toLowerCase(),
        Boolean(request.encrypt), request.keyBase64 || '',
        request.ivBase64 || '', request.dataBase64 || '',
        request.autoPadding !== false);
  });

  const urlModule = {
    URL,
    URLSearchParams: typeof URLSearchParams !== 'undefined' ? URLSearchParams : class {},
    pathToFileURL(p) {
      let str = String(p).replace(/\\/g, '/');
      if (!str.startsWith('/')) str = '/' + str;
      return new URL('file://' + str);
    },
    fileURLToPath(u) {
      const str = typeof u === 'string' ? u : u.href;
      return decodeURIComponent(str.replace(/^file:\/\/\/?/, ''));
    },
    parse(urlStr) {
      try {
        const u = new URL(urlStr);
        return {
          protocol: u.protocol,
          host: u.host,
          hostname: u.hostname,
          port: u.port,
          pathname: u.pathname,
          search: u.search,
          query: u.search ? u.search.slice(1) : '',
          hash: u.hash,
          href: u.href,
        };
      } catch {
        return { href: urlStr };
      }
    },
    format(obj) {
      return obj.href || (obj.protocol ? `${obj.protocol}//${obj.host || ''}${obj.pathname || ''}${obj.search || ''}` : '');
    },
  };

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
  const streamModule = Stream;

  class StringDecoder {
    constructor(encoding = 'utf8') { this.encoding = encoding; }
    write(buf) { return Buffer.from(buf).toString(this.encoding); }
    end(buf) { return buf ? this.write(buf) : ''; }
  }
  const stringDecoderModule = { StringDecoder };

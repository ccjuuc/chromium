  const utilModule = {
    TextEncoder: typeof TextEncoder !== 'undefined' ? TextEncoder : class TextEncoder {
      get encoding() { return 'utf-8'; }
      encode(source = '') { return encodeUtf8(source); }
      encodeInto(source, destination) { return encodeUtf8Into(source, destination); }
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
    promisify: createNodePromisify(),
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
  // This registry is also used for validation: advertise only the canonical
  // algorithms implemented by NativeCryptoDigest, not Node/OpenSSL's wider set.
  const supportedDigestAlgorithms = Object.freeze([
    'md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512',
  ]);
  function createDigestObject(algorithm, key) {
    if (typeof algorithm !== 'string') throw cryptoArgumentError('Algorithm must be a string');
    const normalized = algorithm.toLowerCase().replace(/-/g, '');
    if (!supportedDigestAlgorithms.includes(normalized)) {
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
  function fillMainSecureRandom(bytes) {
    if (typeof globalThis.__xenonCryptoRandom !== 'function') {
      throw Object.assign(new Error('A secure random source is not available'),
                          {code: 'ERR_NOT_SUPPORTED'});
    }
    if (bytes.byteLength) bytes.set(__xenonCryptoRandom(bytes.byteLength));
  }
  const cryptoModule = {
    getHashes() {
      return supportedDigestAlgorithms.slice();
    },
    timingSafeEqual(a, b) {
      if (typeof globalThis.__xenonCryptoTimingSafeEqual !== 'function') {
        throw Object.assign(new Error('Native constant-time comparison is unavailable'),
                            {code: 'ERR_NOT_SUPPORTED'});
      }
      return globalThis.__xenonCryptoTimingSafeEqual(a, b);
    },
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
    randomFillSync(buf, offset = 0, size) {
      fillMainSecureRandom(randomTargetBytes(buf, offset, size));
      return buf;
    },
    randomFill(buf, offset, size, callback) {
      if (typeof offset === 'function') {
        callback = offset; offset = 0; size = undefined;
      } else if (typeof size === 'function') {
        callback = size; size = undefined;
      }
      if (typeof callback !== 'function') throw cryptoArgumentError('Callback must be a function');
      const bytes = randomTargetBytes(buf, offset, size);
      queueMicrotask(() => {
        try { fillMainSecureRandom(bytes); }
        catch (error) { callback(error); return; }
        callback(null, buf);
      });
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
      const windows = __xenonPlatform === 'win32';
      let str = String(p);
      if (windows) str = str.replace(/\\/g, '/');
      // A filename's percent signs, query markers and fragments are literal
      // path bytes. Escape them before URL parses its query/hash components.
      str = encodeURI(str).replace(/\?/g, '%3F').replace(/#/g, '%23');
      if (windows && str.startsWith('//')) return new URL('file:' + str);
      if (!str.startsWith('/')) str = '/' + str;
      return new URL('file://' + str);
    },
    fileURLToPath(input, options) {
      const fail = (code, message) => Object.assign(new TypeError(message), {code});
      if (typeof input !== 'string' && !(input instanceof URL)) {
        throw fail('ERR_INVALID_ARG_TYPE', 'path must be a string or URL');
      }
      if (typeof input === 'string' && /^file:\/\/[^/?#]*@/i.test(input.replace(/\\/g, '/'))) {
        throw fail('ERR_INVALID_URL', 'Invalid file URL authority');
      }
      let url;
      try { url = typeof input === 'string' ? new URL(input) : input; }
      catch (_error) { throw fail('ERR_INVALID_URL', 'Invalid URL'); }
      if (url.protocol.toLowerCase() !== 'file:') throw fail('ERR_INVALID_URL_SCHEME', 'URL must be of scheme file');
      if (url.username || url.password || url.port) throw fail('ERR_INVALID_URL', 'Invalid file URL authority');
      let windows = __xenonPlatform === 'win32';
      if (options && options.windows !== undefined && options.windows !== null) {
        if (typeof options.windows !== 'boolean') {
          throw fail('ERR_INVALID_ARG_TYPE', 'options.windows must be a boolean');
        }
        windows = options.windows;
      }
      if ((windows ? /%(?:2f|5c)/i : /%2f/i).test(url.pathname)) {
        throw fail('ERR_INVALID_FILE_URL_PATH', 'File URL must not include encoded path separators');
      }
      let pathname = decodeURIComponent(url.pathname);
      const hostname = url.hostname.toLowerCase() === 'localhost' ? '' : url.hostname;
      if (!windows) {
        if (hostname) throw fail('ERR_INVALID_FILE_URL_HOST', 'File URL host must be empty or localhost');
        return pathname;
      }
      pathname = pathname.replace(/\//g, '\\');
      if (hostname) {
        // Match the renderer URL adapter: Node decodes IDN labels when forming
        // a Windows UNC filename, even though URL.hostname uses ASCII labels.
        const unicodeHost = hostname.split('.').map(label => {
          if (!label.startsWith('xn--')) return label;
          const encoded = label.slice(4);
          if (!encoded) throw fail('ERR_INVALID_URL', 'Invalid URL hostname');
          const delimiter = encoded.lastIndexOf('-');
          const output = delimiter < 0 ? [] : Array.from(encoded.slice(0, delimiter), c => c.codePointAt(0));
          let cursor = delimiter < 0 ? 0 : delimiter + 1;
          let n = 128, index = 0, bias = 72;
          while (cursor < encoded.length) {
            const oldIndex = index;
            let weight = 1;
            for (let k = 36;; k += 36) {
              if (cursor >= encoded.length) throw fail('ERR_INVALID_URL', 'Invalid URL hostname');
              const code = encoded.charCodeAt(cursor++);
              const digit = code >= 48 && code <= 57 ? code - 22 :
                  code >= 65 && code <= 90 ? code - 65 : code - 97;
              if (digit < 0 || digit >= 36) throw fail('ERR_INVALID_URL', 'Invalid URL hostname');
              index += digit * weight;
              if (!Number.isSafeInteger(index)) throw fail('ERR_INVALID_URL', 'Invalid URL hostname');
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
            if (n > 0x10ffff || (n >= 0xd800 && n <= 0xdfff)) {
              throw fail('ERR_INVALID_URL', 'Invalid URL hostname');
            }
            index %= size;
            output.splice(index++, 0, n);
          }
          return String.fromCodePoint(...output);
        }).join('.');
        return '\\\\' + unicodeHost + pathname;
      }
      if (!/^\\[a-zA-Z]:/.test(pathname)) {
        throw fail('ERR_INVALID_FILE_URL_PATH', 'File URL path must be absolute');
      }
      return pathname.slice(1);
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
    format(obj, options) {
      if (typeof obj === 'string') return urlModule.parse(obj).href;
      if (!obj || typeof obj !== 'object') {
        throw Object.assign(new TypeError('urlObject must be an object or string'), {code: 'ERR_INVALID_ARG_TYPE'});
      }
      if (obj instanceof URL) {
        if (options != null && (typeof options !== 'object' || Array.isArray(options)))
          throw Object.assign(new TypeError('options must be an object'), {code: 'ERR_INVALID_ARG_TYPE'});
        if (options?.unicode)
          throw Object.assign(new Error('Unicode URL formatting is not supported'), {code: 'ERR_NOT_SUPPORTED'});
        const url = new URL(obj.href);
        if (options?.auth != null && !options.auth) { url.username = ''; url.password = ''; }
        if (options?.fragment != null && !options.fragment) url.hash = '';
        if (options?.search != null && !options.search) url.search = '';
        return url.href;
      }
      // Legacy url.format uses fields rather than href, and query objects use
      // querystring encoding (including repeated array keys and %20 spaces).
      const primitive = value => {
        if (typeof value === 'string' || typeof value === 'boolean' || typeof value === 'bigint') return String(value);
        return typeof value === 'number' && Number.isFinite(value) ? String(value) : '';
      };
      const query = obj.query && typeof obj.query === 'object' ? Object.keys(obj.query).flatMap(key => {
        const value = obj.query[key];
        return (Array.isArray(value) ? value : [value]).map(item =>
          encodeURIComponent(key) + '=' + encodeURIComponent(primitive(item)));
      }).join('&') : '';
      let protocol = obj.protocol || '';
      if (protocol && !protocol.endsWith(':')) protocol += ':';
      let pathname = obj.pathname || '';
      let search = obj.search || (query ? '?' + query : '');
      let hash = obj.hash || '';
      const auth = obj.auth ? encodeURIComponent(obj.auth).replace(/%3A/gi, ':') + '@' : '';
      let host = '';
      if (obj.host) host = auth + obj.host;
      else if (obj.hostname) {
        const hostname = obj.hostname;
        host = auth + (hostname.includes(':') && !(hostname.startsWith('[') && hostname.endsWith(']')) ? '[' + hostname + ']' : hostname);
        if (obj.port) host += ':' + obj.port;
      }
      pathname = pathname.replace(/[?#]/g, character => character === '?' ? '%3F' : '%23');
      if (obj.slashes || ['http:', 'https:', 'ftp:', 'gopher:', 'file:', 'ws:', 'wss:'].includes(protocol)) {
        if (obj.slashes || host) {
          if (pathname && !pathname.startsWith('/')) pathname = '/' + pathname;
          host = '//' + host;
        } else if (protocol.startsWith('file')) host = '//';
      }
      search = search.replace(/#/g, '%23');
      if (search && !search.startsWith('?')) search = '?' + search;
      if (hash && !hash.startsWith('#')) hash = '#' + hash;
      return protocol + host + pathname + search + hash;
    },
  };

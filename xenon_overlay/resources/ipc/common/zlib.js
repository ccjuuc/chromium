  function createZlibModule(call) {
    const constants = {
      Z_NO_FLUSH: 0, Z_PARTIAL_FLUSH: 1, Z_SYNC_FLUSH: 2, Z_FULL_FLUSH: 3,
      Z_FINISH: 4, Z_BLOCK: 5, Z_TREES: 6, Z_OK: 0, Z_STREAM_END: 1,
      Z_NEED_DICT: 2, Z_ERRNO: -1, Z_STREAM_ERROR: -2, Z_DATA_ERROR: -3,
      Z_MEM_ERROR: -4, Z_BUF_ERROR: -5, Z_VERSION_ERROR: -6,
      Z_NO_COMPRESSION: 0, Z_BEST_SPEED: 1, Z_BEST_COMPRESSION: 9,
      Z_DEFAULT_COMPRESSION: -1, Z_FILTERED: 1, Z_HUFFMAN_ONLY: 2, Z_RLE: 3,
      Z_FIXED: 4, Z_DEFAULT_STRATEGY: 0, Z_MIN_WINDOWBITS: 8, Z_MAX_WINDOWBITS: 15,
      Z_DEFAULT_WINDOWBITS: 15, Z_MIN_CHUNK: 64, Z_DEFAULT_CHUNK: 16384,
      Z_MIN_MEMLEVEL: 1, Z_MAX_MEMLEVEL: 9, Z_DEFAULT_MEMLEVEL: 8,
      Z_MIN_LEVEL: -1, Z_MAX_LEVEL: 9, Z_DEFAULT_LEVEL: -1,
    };
    function codedError(code, message, Type = Error) {
      const error = new Type(message);
      error.code = code;
      return error;
    }
    function nativeError(error) {
      if (error && error.code) return error;
      const message = String(error && error.message || error);
      const code = /(?:^|\b)(Z_[A-Z_]+|ERR_[A-Z_]+):/.exec(message);
      return codedError(code ? code[1] : 'ERR_OPERATION_FAILED', message);
    }
    function bytes(input) {
      if (typeof input === 'string') return Buffer.from(input);
      if (ArrayBuffer.isView(input)) {
        return Buffer.from(new Uint8Array(input.buffer, input.byteOffset, input.byteLength));
      }
      if (input instanceof ArrayBuffer) return Buffer.from(new Uint8Array(input));
      throw codedError('ERR_INVALID_ARG_TYPE', 'zlib input must be a string or binary buffer', TypeError);
    }
    function options(value) {
      if (value === undefined || value === null) return {};
      if (typeof value !== 'object' || Array.isArray(value)) {
        throw codedError('ERR_INVALID_ARG_TYPE', 'zlib options must be an object', TypeError);
      }
      const result = {};
      const ranges = {level: [-1, 9], windowBits: [9, 15], memLevel: [1, 9],
        strategy: [0, 4], chunkSize: [64, 2147483647], maxOutputLength: [1, 67108864]};
      for (const [key, setting] of Object.entries(value)) {
        const value = setting;
        if (value === undefined) continue;
        if (key === 'flush' || key === 'finishFlush') {
          if (value !== (key === 'flush' ? 0 : 4)) {
            throw codedError('ERR_NOT_SUPPORTED', 'Partial zlib flush is not supported');
          }
        } else if (!ranges[key]) {
          throw codedError('ERR_NOT_SUPPORTED', 'Unsupported zlib option: ' + key);
        } else if (!Number.isInteger(value) || value < ranges[key][0] || value > ranges[key][1]) {
          throw codedError('ERR_OUT_OF_RANGE', 'Invalid zlib option: ' + key, RangeError);
        }
        result[key] = value;
      }
      return result;
    }
    const codes = {};
    for (const name of ['Z_OK', 'Z_STREAM_END', 'Z_NEED_DICT', 'Z_ERRNO',
        'Z_STREAM_ERROR', 'Z_DATA_ERROR', 'Z_MEM_ERROR', 'Z_BUF_ERROR', 'Z_VERSION_ERROR']) {
      codes[name] = constants[name];
      codes[constants[name]] = name;
    }
    const result = Object.assign({constants, codes}, constants);
    for (const name of ['gzip', 'gunzip', 'deflate', 'inflate', 'deflateRaw', 'inflateRaw', 'unzip']) {
      result[name + 'Sync'] = (input, opts) => {
        const data = bytes(input), settings = options(opts);
        try { return Buffer.from(call(name, data, settings, false)); }
        catch (error) { throw nativeError(error); }
      };
      result[name] = (input, opts, callback) => {
        if (typeof opts === 'function') { callback = opts; opts = undefined; }
        if (typeof callback !== 'function') {
          throw codedError('ERR_INVALID_ARG_TYPE', 'zlib callback must be a function', TypeError);
        }
        const data = bytes(input), settings = options(opts);
        let pending;
        try { pending = call(name, data, settings, true); }
        catch (error) { queueMicrotask(() => callback(nativeError(error))); return; }
        Promise.resolve(pending).then(value => callback(null, Buffer.from(value)),
                                      error => callback(nativeError(error)));
      };
    }
    for (const name of ['Gzip', 'Gunzip', 'Deflate', 'Inflate', 'DeflateRaw', 'InflateRaw', 'Unzip',
        'BrotliCompress', 'BrotliDecompress']) {
      const unsupported = function() {
        throw codedError('ERR_NOT_SUPPORTED', 'zlib.' + name + ' streaming is not supported');
      };
      result[name] = unsupported;
      result['create' + name] = unsupported;
    }
    for (const name of ['brotliCompress', 'brotliDecompress']) {
      result[name + 'Sync'] = () => {
        throw codedError('ERR_NOT_SUPPORTED', 'zlib.' + name + ' is not supported');
      };
      result[name] = (input, opts, callback) => {
        if (typeof opts === 'function') callback = opts;
        if (typeof callback !== 'function') {
          throw codedError('ERR_INVALID_ARG_TYPE', 'zlib callback must be a function', TypeError);
        }
        queueMicrotask(() => callback(codedError('ERR_NOT_SUPPORTED', 'zlib.' + name + ' is not supported')));
      };
    }
    return result;
  }

  function createZlibModule(call, dependencies = {}) {
    const constants = {
      Z_NO_FLUSH: 0, Z_PARTIAL_FLUSH: 1, Z_SYNC_FLUSH: 2, Z_FULL_FLUSH: 3,
      Z_FINISH: 4, Z_BLOCK: 5, Z_TREES: 6, Z_OK: 0, Z_STREAM_END: 1,
      Z_NEED_DICT: 2, Z_ERRNO: -1, Z_STREAM_ERROR: -2, Z_DATA_ERROR: -3,
      Z_MEM_ERROR: -4, Z_BUF_ERROR: -5, Z_VERSION_ERROR: -6,
      Z_NO_COMPRESSION: 0, Z_BEST_SPEED: 1, Z_BEST_COMPRESSION: 9,
      Z_DEFAULT_COMPRESSION: -1, Z_FILTERED: 1, Z_HUFFMAN_ONLY: 2, Z_RLE: 3,
      Z_FIXED: 4, Z_DEFAULT_STRATEGY: 0, Z_MIN_WINDOWBITS: 8, Z_MAX_WINDOWBITS: 15,
      Z_DEFAULT_WINDOWBITS: 15, Z_MIN_CHUNK: 64, Z_MAX_CHUNK: Infinity, Z_DEFAULT_CHUNK: 16384,
      Z_MIN_MEMLEVEL: 1, Z_MAX_MEMLEVEL: 9, Z_DEFAULT_MEMLEVEL: 8,
      Z_MIN_LEVEL: -1, Z_MAX_LEVEL: 9, Z_DEFAULT_LEVEL: -1,
      BROTLI_OPERATION_PROCESS: 0, BROTLI_OPERATION_FLUSH: 1, BROTLI_OPERATION_FINISH: 2,
      BROTLI_OPERATION_EMIT_METADATA: 3, BROTLI_PARAM_MODE: 0, BROTLI_MODE_GENERIC: 0,
      BROTLI_MODE_TEXT: 1, BROTLI_MODE_FONT: 2, BROTLI_DEFAULT_MODE: 0,
      BROTLI_PARAM_QUALITY: 1, BROTLI_MIN_QUALITY: 0, BROTLI_MAX_QUALITY: 11,
      BROTLI_DEFAULT_QUALITY: 11, BROTLI_PARAM_LGWIN: 2, BROTLI_MIN_WINDOW_BITS: 10,
      BROTLI_MAX_WINDOW_BITS: 24, BROTLI_LARGE_MAX_WINDOW_BITS: 30, BROTLI_DEFAULT_WINDOW: 22,
      BROTLI_PARAM_LGBLOCK: 3, BROTLI_MIN_INPUT_BLOCK_BITS: 16, BROTLI_MAX_INPUT_BLOCK_BITS: 24,
      BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING: 4, BROTLI_PARAM_SIZE_HINT: 5,
      BROTLI_PARAM_LARGE_WINDOW: 6, BROTLI_PARAM_NPOSTFIX: 7, BROTLI_PARAM_NDIRECT: 8,
      BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION: 0, BROTLI_DECODER_PARAM_LARGE_WINDOW: 1,
      BROTLI_DECODER_RESULT_ERROR: 0, BROTLI_DECODER_RESULT_SUCCESS: 1,
      BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT: 2, BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT: 3,
      ZSTD_e_continue: 0, ZSTD_e_flush: 1, ZSTD_e_end: 2,
      ZSTD_fast: 1, ZSTD_dfast: 2, ZSTD_greedy: 3, ZSTD_lazy: 4, ZSTD_lazy2: 5,
      ZSTD_btlazy2: 6, ZSTD_btopt: 7, ZSTD_btultra: 8, ZSTD_btultra2: 9,
      ZSTD_c_compressionLevel: 100, ZSTD_c_windowLog: 101, ZSTD_c_hashLog: 102,
      ZSTD_c_chainLog: 103, ZSTD_c_searchLog: 104, ZSTD_c_minMatch: 105,
      ZSTD_c_targetLength: 106, ZSTD_c_strategy: 107, ZSTD_c_targetCBlockSize: 130,
      ZSTD_c_enableLongDistanceMatching: 160, ZSTD_c_ldmHashLog: 161,
      ZSTD_c_ldmMinMatch: 162, ZSTD_c_ldmBucketSizeLog: 163, ZSTD_c_ldmHashRateLog: 164,
      ZSTD_c_contentSizeFlag: 200, ZSTD_c_checksumFlag: 201, ZSTD_c_dictIDFlag: 202,
      ZSTD_c_nbWorkers: 400, ZSTD_c_jobSize: 401, ZSTD_c_overlapLog: 402,
      ZSTD_d_windowLogMax: 100, ZSTD_CLEVEL_DEFAULT: 3,
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
    const streamOptions = new Set(['highWaterMark', 'readableHighWaterMark', 'writableHighWaterMark',
      'encoding', 'defaultEncoding', 'decodeStrings', 'autoDestroy', 'emitClose', 'signal',
      'objectMode', 'readableObjectMode', 'writableObjectMode']);
    function options(value, operation) {
      if (value === undefined || value === null) return {};
      if (typeof value !== 'object' || Array.isArray(value)) {
        throw codedError('ERR_INVALID_ARG_TYPE', 'zlib options must be an object', TypeError);
      }
      const result = {};
      const extended = operation.startsWith('brotli') || operation.startsWith('zstd');
      const ranges = {level: [-1, 9], windowBits: [8, 15], memLevel: [1, 9],
        strategy: [0, 4], chunkSize: [64, 2147483647], maxOutputLength: [1, 67108864],
        flush: [0, extended ? (operation.startsWith('brotli') ? 3 : 2) : 5],
        finishFlush: [0, extended ? (operation.startsWith('brotli') ? 3 : 2) : 5]};
      for (const [key, setting] of Object.entries(value)) {
        if (setting === undefined || streamOptions.has(key)) continue;
        if (key === 'dictionary') {
          result[key] = bytes(setting);
        } else if (key === 'params' && extended) {
          if (!setting || typeof setting !== 'object' || Array.isArray(setting)) {
            throw codedError('ERR_INVALID_ARG_TYPE', 'Codec params must be an object', TypeError);
          }
          result.params = {};
          for (const [parameter, raw] of Object.entries(setting)) {
            const number = typeof raw === 'boolean' ? Number(raw) : raw;
            if (!/^\d+$/.test(parameter) || !Number.isInteger(number) || number < -2147483648 || number > 2147483647) {
              throw codedError('ERR_OUT_OF_RANGE', 'Codec parameters must have integer keys and values', RangeError);
            }
            result.params[parameter] = number;
          }
        } else if (!ranges[key]) {
          throw codedError('ERR_NOT_SUPPORTED', 'Unsupported zlib option: ' + key);
        } else if (key === 'windowBits' && setting === 0 &&
                   ['inflate', 'gunzip', 'unzip'].includes(operation)) {
          result[key] = setting;
        } else if (!Number.isInteger(setting) || setting < ranges[key][0] || setting > ranges[key][1]) {
          throw codedError('ERR_OUT_OF_RANGE', 'Invalid zlib option: ' + key, RangeError);
        } else {
          result[key] = setting;
        }
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
    const formats = ['gzip', 'gunzip', 'deflate', 'inflate', 'deflateRaw', 'inflateRaw', 'unzip',
      'brotliCompress', 'brotliDecompress', 'zstdCompress', 'zstdDecompress'];
    for (const name of formats) {
      result[name + 'Sync'] = (input, opts) => {
        const data = bytes(input), settings = options(opts, name);
        try { return Buffer.from(call(name, data, settings, false)); }
        catch (error) { throw nativeError(error); }
      };
      result[name] = (input, opts, callback) => {
        if (typeof opts === 'function') { callback = opts; opts = undefined; }
        if (typeof callback !== 'function') {
          throw codedError('ERR_INVALID_ARG_TYPE', 'zlib callback must be a function', TypeError);
        }
        const data = bytes(input), settings = options(opts, name);
        let pending;
        try { pending = call(name, data, settings, true); }
        catch (error) { queueMicrotask(() => callback(nativeError(error))); return; }
        Promise.resolve(pending).then(value => callback(null, Buffer.from(value)),
                                      error => callback(nativeError(error)));
      };
    }
    const {Transform, streamCall} = dependencies;
    const flushMarkers = new WeakMap();
    const finalizer = typeof FinalizationRegistry === 'function' && typeof streamCall === 'function'
      ? new FinalizationRegistry(id => { try { streamCall({action: 'close', id}, false); } catch (_) {} })
      : null;
    // A single write remains pending until its bounded native requests and
    // readable backpressure complete. No whole-stream input/output collection.
    const BaseTransform = Transform || class {};
    class Codec extends BaseTransform {
      constructor(operation, opts) {
        if (typeof Transform !== 'function' || typeof streamCall !== 'function') {
          throw codedError('ERR_NOT_SUPPORTED', 'Native compression streams are unavailable');
        }
        const settings = options(opts, operation);
        super({...opts, objectMode: false, readableObjectMode: false, writableObjectMode: false,
          decodeStrings: true, autoDestroy: true});
        this._codecOperation = operation;
        this._codecSettings = settings;
        this._codecExtended = operation.startsWith('brotli') || operation.startsWith('zstd');
        this._codecFinish = settings.finishFlush === undefined ? (this._codecExtended ? 2 : 4) : settings.finishFlush;
        this._codecChunkSize = Math.min(settings.chunkSize || 16384, 65536);
        this._codecPending = null;
        this._codecWorking = false;
        this._codecPaused = false;
        this.bytesWritten = 0;
        try { this._codecId = streamCall({action: 'create', operation, options: settings}, false); }
        catch (error) { throw nativeError(error); }
        if (finalizer) finalizer.register(this, this._codecId, this);
      }
      get bytesRead() { return this.bytesWritten; }
      _transform(chunk, encoding, callback) {
        const marker = flushMarkers.get(chunk);
        this._codecStart(chunk, marker === undefined ? (this._codecSettings.flush || 0) : marker, callback);
      }
      _flush(callback) { this._codecStart(Buffer.alloc(0), this._codecFinish, callback); }
      _codecStart(input, flush, callback) {
        this._codecPending = {input, offset: 0, flush, callback, complete: false};
        this._codecPump();
      }
      _read(size) {
        this._codecPaused = false;
        super._read(size);
        this._codecPump();
      }
      async _codecPump() {
        if (this._codecWorking || this._codecPaused || !this._codecPending || this.destroyed) return;
        this._codecWorking = true;
        try {
          while (this._codecPending && !this._codecPaused && !this.destroyed) {
            const pending = this._codecPending;
            if (pending.complete) {
              this._codecPending = null;
              pending.callback();
              continue;
            }
            const data = pending.input.subarray(pending.offset, pending.offset + 65536);
            const last = pending.offset + data.byteLength === pending.input.byteLength;
            const response = await streamCall({action: 'process', id: this._codecId, data,
              flush: last ? pending.flush : 0, outputSize: this._codecChunkSize}, true);
            if (this.destroyed || this._codecPending !== pending) break;
            const consumed = response.consumed;
            const output = Buffer.from(response.output);
            if (!Number.isInteger(consumed) || consumed < 0 || consumed > data.byteLength || output.length > this._codecChunkSize) {
              throw codedError('ERR_OPERATION_FAILED', 'Invalid native compression response');
            }
            pending.offset += consumed;
            this.bytesWritten += consumed;
            pending.complete = pending.offset === pending.input.byteLength && response.done;
            if (!consumed && !output.length && !response.done) {
              throw codedError('Z_BUF_ERROR', 'Compression codec made no progress');
            }
            if (output.length && !this.push(output)) this._codecPaused = true;
          }
        } catch (error) {
          const pending = this._codecPending;
          this._codecPending = null;
          if (pending) pending.callback(nativeError(error));
          else if (!this.destroyed) this.destroy(nativeError(error));
        } finally {
          this._codecWorking = false;
        }
      }
      _destroy(error, callback) {
        if (finalizer) finalizer.unregister(this);
        const id = this._codecId;
        this._codecId = undefined;
        if (id !== undefined) {
          try { streamCall({action: 'close', id}, false); }
          catch (failure) { if (!error) error = nativeError(failure); }
        }
        const pending = this._codecPending;
        this._codecPending = null;
        if (pending) pending.callback(error || codedError('ERR_STREAM_DESTROYED', 'Compression stream was destroyed'));
        callback(error);
      }
      flush(kind, callback) {
        if (typeof kind === 'function') { callback = kind; kind = undefined; }
        if (kind === undefined) kind = this._codecExtended ? 1 : 3;
        const maximum = this._codecOperation.startsWith('brotli') ? 3 : this._codecExtended ? 2 : 5;
        if (!Number.isInteger(kind) || kind < 0 || kind > maximum) {
          throw codedError('ERR_OUT_OF_RANGE', 'Invalid compression flush operation', RangeError);
        }
        if (this.writableEnded || this.destroyed) {
          if (callback) queueMicrotask(() => callback());
          return;
        }
        const marker = Buffer.alloc(0);
        flushMarkers.set(marker, kind);
        this.write(marker, callback);
      }
      close(callback) {
        if (callback) {
          if (this.closed) queueMicrotask(callback);
          else this.once('close', callback);
        }
        this.destroy();
      }
      reset() { throw codedError('ERR_NOT_SUPPORTED', 'Resetting compression streams is not supported'); }
      params(level, strategy, callback) {
        const error = codedError('ERR_NOT_SUPPORTED', 'Changing compression parameters after construction is not supported');
        if (typeof callback === 'function') queueMicrotask(() => callback(error));
        else throw error;
      }
    }
    for (const operation of formats) {
      const name = operation[0].toUpperCase() + operation.slice(1);
      const Constructor = function(opts) {
        const stream = new Codec(operation, opts);
        Object.setPrototypeOf(stream, Constructor.prototype);
        return stream;
      };
      Object.defineProperty(Constructor, 'name', {value: name});
      Constructor.prototype = Object.create(Codec.prototype, {constructor: {value: Constructor, writable: true, configurable: true}});
      result[name] = Constructor;
      result['create' + name] = opts => new Constructor(opts);
    }
    return result;
  }

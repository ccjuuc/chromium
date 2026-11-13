// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function installFileStreams(fsModule, readRange, unsupported) {
  const invalid = message => Object.assign(new RangeError(message), {code: 'ERR_OUT_OF_RANGE'});
  function optionsObject(options) {
    if (typeof options === 'string') return {encoding: options};
    if (!options || typeof options !== 'object') throw new TypeError('Invalid file stream options');
    return options;
  }
  class FileReadStream extends Readable {
    constructor(path, options = {}) {
      options = optionsObject(options);
      if (options.fd != null || options.fs || (options.flags && options.flags !== 'r') ||
          options.autoClose === false) return unsupported('ReadStream options');
      const start = options.start === undefined ? 0 : options.start;
      const end = options.end === undefined ? Infinity : options.end;
      const highWaterMark = options.highWaterMark === undefined ? 65536 : options.highWaterMark;
      if (!Number.isSafeInteger(start) || start < 0 ||
          (end !== Infinity && (!Number.isSafeInteger(end) || end < start)) ||
          !Number.isSafeInteger(highWaterMark) || highWaterMark <= 0)
        throw invalid('Invalid ReadStream range or highWaterMark');
      super({highWaterMark, encoding: options.encoding, signal: options.signal,
        emitClose: options.emitClose !== false, autoDestroy: true});
      this.path = path;
      this.pending = true;
      this.bytesRead = 0;
      this._start = start;
      this._end = end;
      this._bytes = null;
      this._position = 0;
      this._closeCallbacks = [];
    }
    _construct(callback) {
      Promise.resolve().then(() => this.destroyed ? null : readRange(this.path, this._start, this._end))
          .then(bytes => {
            this.pending = false;
            if (!this.destroyed) {
              this._bytes = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes);
              this.emit('ready');
            }
            callback();
          }, error => { this.pending = false; callback(error); });
    }
    _read(size) {
      // File reads complete asynchronously. In particular, a decoder may
      // consume a partial UTF-8 sequence without producing any readable bytes;
      // asynchronous completion lets Readable request its next chunk normally.
      queueMicrotask(() => {
        if (this.destroyed || !this._bytes) return;
        if (this._position >= this._bytes.length) {
          this._bytes = null;
          this.push(null);
          return;
        }
        const end = Math.min(this._position + size, this._bytes.length);
        const chunk = this._bytes.subarray(this._position, end);
        this._position = end;
        this.bytesRead += chunk.length;
        this.push(chunk);
      });
    }
    _destroy(error, callback) {
      this.pending = false;
      this._bytes = null;
      callback(error);
      for (const done of this._closeCallbacks.splice(0)) queueMicrotask(done);
    }
    close(callback) {
      if (callback !== undefined && typeof callback !== 'function')
        throw Object.assign(new TypeError('callback must be a function'), {code: 'ERR_INVALID_ARG_TYPE'});
      if (callback) {
        if (this.closed) queueMicrotask(callback);
        else this._closeCallbacks.push(callback);
      }
      return this.destroy();
    }
  }
  class FileWriteStream extends Writable {
    constructor(path, options = {}) {
      options = optionsObject(options);
      if (options.fd != null || options.fs || options.start !== undefined ||
          options.autoClose === false || options.flush || options.objectMode ||
          (options.mode !== undefined && options.mode !== 0o666))
        return unsupported('WriteStream options');
      const flags = options.flags === undefined ? 'w' : options.flags;
      if (flags !== 'w' && flags !== 'a') return unsupported('WriteStream flags ' + flags);
      const highWaterMark = options.highWaterMark === undefined ? 16384 : options.highWaterMark;
      if (!Number.isSafeInteger(highWaterMark) || highWaterMark < 0)
        throw invalid('Invalid WriteStream highWaterMark');
      super({highWaterMark, defaultEncoding: options.encoding || 'utf8',
        signal: options.signal, emitClose: options.emitClose !== false, autoDestroy: true});
      this.setDefaultEncoding(options.encoding === undefined ? 'utf8' : options.encoding);
      this.path = path;
      this.fd = null;
      this.pending = true;
      this.bytesWritten = 0;
      this._flags = flags;
      this._activeWrite = null;
      this._closeCallbacks = [];
    }
    _construct(callback) {
      Promise.resolve().then(() => {
        if (this.destroyed) return;
        return this._flags === 'a' ? fsModule.promises.appendFile(this.path, Buffer.alloc(0)) :
          fsModule.promises.writeFile(this.path, Buffer.alloc(0));
      }).then(() => {
        this.pending = false;
        if (!this.destroyed) this.emit('ready');
        callback();
      }, error => { this.pending = false; callback(error); });
    }
    write(chunk, encoding, callback) {
      // The filesystem bridge snapshots write data before asynchronous IPC.
      if (ArrayBuffer.isView(chunk))
        chunk = Buffer.from(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
      return super.write(chunk, encoding, callback);
    }
    end(chunk, encoding, callback) {
      if (ArrayBuffer.isView(chunk))
        chunk = Buffer.from(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
      return super.end(chunk, encoding, callback);
    }
    _write(chunk, encoding, callback) {
      this._activeWrite = Promise.resolve().then(() => fsModule.promises.appendFile(this.path, chunk));
      this._activeWrite.then(() => {
        this._activeWrite = null;
        this.bytesWritten += chunk.length;
        callback(this.destroyed ? this.errored ||
          Object.assign(new Error('Stream was destroyed'), {code: 'ERR_STREAM_DESTROYED'}) : undefined);
      }, error => { this._activeWrite = null; callback(error); });
    }
    _destroy(error, callback) {
      // An in-flight bridge write cannot be cancelled. Delay close until that
      // operation completes so observers never see writes after close.
      const finish = () => {
        this.pending = false;
        callback(error);
        for (const done of this._closeCallbacks.splice(0)) queueMicrotask(done);
      };
      if (this._activeWrite) this._activeWrite.then(finish, finish);
      else finish();
    }
    close(callback) {
      if (callback !== undefined && typeof callback !== 'function')
        throw Object.assign(new TypeError('callback must be a function'), {code: 'ERR_INVALID_ARG_TYPE'});
      if (callback) {
        if (this.closed) queueMicrotask(callback);
        else this._closeCallbacks.push(callback);
      }
      if (!this.destroyed) this.end();
      return this;
    }
  }
  fsModule.ReadStream = FileReadStream;
  fsModule.WriteStream = FileWriteStream;
  fsModule.createReadStream = (path, options) => new FileReadStream(path, options);
  fsModule.createWriteStream = (path, options) => new FileWriteStream(path, options);
}

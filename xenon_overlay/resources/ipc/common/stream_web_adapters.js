// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// These adapters use the public queue/lifecycle contracts of both families.
// In particular, a write callback waits for the real Web writer and a full
// Web queue pauses its Node producer. No complete body is buffered here.
function createStreamWebAdapters({Readable, Writable, Duplex, Buffer, webStreams}) {
  const {ReadableStream, WritableStream} = webStreams;
  const readableLocked = Object.getOwnPropertyDescriptor(ReadableStream.prototype, 'locked').get;
  const writableLocked = Object.getOwnPropertyDescriptor(WritableStream.prototype, 'locked').get;
  const error = (code, message, Type = Error) => Object.assign(new Type(message), {code});
  const invalid = message => error('ERR_INVALID_ARG_TYPE', message, TypeError);
  const premature = () => error('ERR_STREAM_PREMATURE_CLOSE', 'Premature close');
  const aborted = () => Object.assign(error('ABORT_ERR', 'The operation was aborted'),
      {name: 'AbortError'});
  const deferred = () => {
    let resolve, reject;
    const promise = new Promise((a, b) => { resolve = a; reject = b; });
    return {promise, resolve, reject};
  };
  const isReadable = value => {
    try { readableLocked.call(value); return true; } catch (_) { return false; }
  };
  const isWritable = value => {
    try { writableLocked.call(value); return true; } catch (_) { return false; }
  };
  const optionsObject = value => {
    if (value === undefined) return {};
    if (value === null || typeof value !== 'object' || Array.isArray(value))
      throw invalid('options must be an object');
    return value;
  };
  const nodeOptions = value => {
    const options = optionsObject(value);
    for (const name of ['objectMode', 'decodeStrings', 'allowHalfOpen']) {
      if (options[name] !== undefined && typeof options[name] !== 'boolean')
        throw invalid('options.' + name + ' must be a boolean');
    }
    if (options.encoding !== undefined && !Buffer.isEncoding(options.encoding))
      throw error('ERR_INVALID_ARG_VALUE', 'Invalid options.encoding', TypeError);
    if (options.highWaterMark !== undefined &&
        (!Number.isInteger(options.highWaterMark) || options.highWaterMark < 0))
      throw error('ERR_INVALID_ARG_VALUE', 'Invalid options.highWaterMark', TypeError);
    if (options.signal !== undefined && (!options.signal ||
        typeof options.signal.aborted !== 'boolean' ||
        typeof options.signal.addEventListener !== 'function'))
      throw invalid('options.signal must be an AbortSignal');
    return options;
  };
  // Node callbacks can synchronously throw (for example from an error event).
  // Do not convert those throws into ignored Promise rejections.
  const callback = (done, failure) => {
    try { done(failure); } catch (thrown) { queueMicrotask(() => { throw thrown; }); }
  };

  function fromWeb(readable, writable, options, Type) {
    options = nodeOptions(options);
    if (readable !== undefined && !isReadable(readable))
      throw invalid('readableStream must be a ReadableStream');
    if (writable !== undefined && !isWritable(writable))
      throw invalid('writableStream must be a WritableStream');
    let reader, writer;
    try {
      reader = readable?.getReader();
      writer = writable?.getWriter();
    } catch (failure) {
      // A locked second half must not leave the first half locked by a failed
      // conversion. Handle closed before releaseLock creates its rejection.
      if (reader) { reader.closed.catch(() => {}); reader.releaseLock(); }
      throw failure;
    }
    let node, readerClosed = false, writerClosed = false, reading = false;
    const completeWrite = (promise, done) => promise.then(
        () => callback(done), failure => callback(done, failure));
    try {
      node = new Type({
        highWaterMark: options.highWaterMark,
        objectMode: options.objectMode ?? false,
        encoding: options.encoding,
        decodeStrings: options.decodeStrings ?? true,
        allowHalfOpen: options.allowHalfOpen ?? false,
        signal: options.signal,
        ...(reader ? {read() {
          if (reading) return;
          reading = true;
          reader.read().then(result => {
            reading = false;
            if (node.destroyed) return;
            try { node.push(result.done ? null : result.value); }
            catch (failure) { node.destroy(failure); }
          }, failure => { reading = false; node.destroy(failure); });
        }} : {}),
        ...(writer ? {
          write(chunk, encoding, done) {
            completeWrite(writer.ready.then(() => writer.write(chunk)), done);
          },
          final(done) {
            if (writerClosed) { callback(done); return; }
            completeWrite(writer.close(), done);
          },
        } : {}),
        destroy(reason, done) {
          const pending = [];
          if (reader && !readerClosed) pending.push(reader.cancel(reason));
          if (writer && !writerClosed) {
            // Writable.destroy() without an error gracefully closes its Web
            // sink; destroying a duplex aborts its remaining writable half.
            pending.push(reason != null || reader ? writer.abort(reason) : writer.close());
          }
          Promise.allSettled(pending).then(() => callback(done, reason));
        },
      });
    } catch (failure) {
      if (reader) { reader.closed.catch(() => {}); reader.releaseLock(); }
      if (writer) { writer.closed.catch(() => {}); writer.releaseLock(); }
      throw failure;
    }
    if (reader) reader.closed.then(() => { readerClosed = true; }, failure => {
      readerClosed = true;
      node.destroy(failure);
    });
    if (writer) writer.closed.then(() => {
      writerClosed = true;
      if (!node.writableEnded && !node.destroyed) node.destroy(premature());
    }, failure => { writerClosed = true; node.destroy(failure); });
    // Like Node's fromWeb, the conversion owns its reader/writer lock even
    // after completion. It must not permit a second consumer to steal it.
    return node;
  }

  function readableFromWeb(stream, options) {
    if (!isReadable(stream)) throw invalid('readableStream must be a ReadableStream');
    return fromWeb(stream, undefined, options, Readable);
  }
  function writableFromWeb(stream, options) {
    if (!isWritable(stream)) throw invalid('writableStream must be a WritableStream');
    return fromWeb(undefined, stream, options, Writable);
  }
  function duplexFromWeb(pair, options) {
    if (pair === null || typeof pair !== 'object') throw invalid('pair must be an object');
    if (!isReadable(pair.readable)) throw invalid('pair.readable must be a ReadableStream');
    if (!isWritable(pair.writable)) throw invalid('pair.writable must be a WritableStream');
    return fromWeb(pair.readable, pair.writable, options, Duplex);
  }

  function readableToWeb(node, options) {
    if (!node || typeof node._readableState !== 'object' || typeof node.pause !== 'function' ||
        typeof node.resume !== 'function' || typeof node.on !== 'function')
      throw invalid('streamReadable must be a stream.Readable');
    options = optionsObject(options);
    if (options.type !== undefined && options.type !== 'bytes')
      throw error('ERR_INVALID_ARG_VALUE', 'options.type must be bytes', TypeError);
    const bytes = options.type === 'bytes';
    if (node.destroyed || node.readable === false) {
      const stream = new ReadableStream({type: bytes ? 'bytes' : undefined});
      stream.cancel();
      return stream;
    }
    const objectMode = node.readableObjectMode;
    const strategy = bytes ? {highWaterMark: node.readableHighWaterMark} :
        options.strategy ?? {highWaterMark: node.readableHighWaterMark,
          size: objectMode ? () => 1 : chunk => chunk?.byteLength ?? chunk?.length ?? 1};
    let controller, settled = false;
    const cleanup = () => {
      node.removeListener('data', onData);
      node.removeListener('end', onEnd);
      node.removeListener('close', onClose);
      // Keep the small error listener after completion: Node streams may emit
      // destroy errors asynchronously, including after Web cancellation.
    };
    const fail = failure => {
      if (settled) return;
      settled = true;
      cleanup();
      controller.error(failure);
    };
    function onData(chunk) {
      if (settled) return;
      try {
        // A byte controller transfers its input; never detach pooled Buffer
        // backing or mutate data still owned by the Node producer.
        if (!objectMode && (Buffer.isBuffer(chunk) || (bytes && ArrayBuffer.isView(chunk))))
          chunk = new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength).slice();
        controller.enqueue(chunk);
        if (controller.desiredSize <= 0) node.pause();
      } catch (failure) { fail(failure); node.destroy(failure); }
    }
    function onEnd() {
      if (settled) return;
      settled = true;
      cleanup();
      controller.close();
      if (bytes) controller.byobRequest?.respond(0);
    }
    function onClose() {
      if (!settled) fail(node.errored || premature());
    }
    node.pause();
    return new ReadableStream({
      type: bytes ? 'bytes' : undefined,
      start(value) {
        controller = value;
        node.on('error', fail);
        node.on('end', onEnd);
        node.on('close', onClose);
        node.on('data', onData);
        if (node.readableEnded) onEnd();
        else if (node.errored) fail(node.errored);
      },
      pull() { if (!settled) node.resume(); },
      cancel(reason) {
        if (settled) return;
        settled = true;
        cleanup();
        node.destroy(reason);
      },
    }, strategy);
  }

  function writableToWeb(node) {
    if (!node || typeof node.write !== 'function' || typeof node.on !== 'function' ||
        typeof node.end !== 'function' || typeof node.destroy !== 'function')
      throw invalid('streamWritable must be a stream.Writable');
    if (node.destroyed || node.writable === false) {
      const stream = new WritableStream();
      stream.close();
      return stream;
    }
    let controller, drain, closing, settled = false;
    const cleanup = () => {
      node.removeListener('drain', onDrain);
      node.removeListener('finish', onFinish);
      node.removeListener('close', onClose);
    };
    const fail = failure => {
      if (settled) return;
      settled = true;
      cleanup();
      drain?.reject(failure);
      closing?.reject(failure);
      controller.error(failure);
    };
    function onDrain() { drain?.resolve(); }
    function onFinish() {
      if (settled) return;
      if (!closing) { fail(aborted()); return; }
      settled = true;
      cleanup();
      drain?.resolve();
      closing.resolve();
    }
    function onClose() { if (!settled) fail(node.errored || premature()); }
    return new WritableStream({
      start(value) {
        controller = value;
        node.on('drain', onDrain);
        node.on('finish', onFinish);
        node.on('close', onClose);
        node.on('error', fail);
      },
      write(chunk) {
        if (!node.writableObjectMode && Object.prototype.toString.call(chunk) === '[object ArrayBuffer]')
          chunk = new Uint8Array(chunk);
        const accepted = node.write(chunk);
        if (!accepted || node.writableNeedDrain) {
          drain = deferred();
          // Some valid streams drain synchronously from their write callback.
          if (!node.writableNeedDrain) drain.resolve();
          return drain.promise.finally(() => { drain = undefined; });
        }
      },
      close() {
        if (settled) return;
        if (!closing) {
          closing = deferred();
          if (node.writableFinished) onFinish();
          else if (!node.writableEnded) node.end();
        }
        return closing.promise;
      },
      abort(reason) {
        if (settled) return;
        settled = true;
        cleanup();
        node.destroy(reason);
      },
    }, {highWaterMark: node.writableHighWaterMark,
      size: node.writableObjectMode ? () => 1 : chunk => chunk?.byteLength ?? chunk?.length ?? 1});
  }

  function duplexToWeb(node, options) {
    if (!node || typeof node._readableState !== 'object' || typeof node._writableState !== 'object')
      throw invalid('duplex must be a stream.Duplex');
    options = optionsObject(options);
    return {readable: readableToWeb(node, {type: options.readableType ?? options.type}),
      writable: writableToWeb(node)};
  }
  return {
    Readable: {fromWeb: readableFromWeb, toWeb: readableToWeb},
    Writable: {fromWeb: writableFromWeb, toWeb: writableToWeb},
    Duplex: {fromWeb: duplexFromWeb, toWeb: duplexToWeb},
  };
}

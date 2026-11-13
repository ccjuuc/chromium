// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Installs only after the independent Utility has inherited real OS stdio.
// The native call uses fd 0/1/2, so native addon writes reach the same pipes.
function installUtilityRuntime({process, Buffer, stream, call, exitHost, execArgv = []}) {
  const unsupported = () => Object.assign(new Error('Utility process transport is unavailable'),
      {code: 'ERR_NOT_SUPPORTED'});
  if (typeof call !== 'function' || typeof exitHost !== 'function') throw unsupported();
  let exitCode, exiting = false;
  Object.defineProperty(process, 'exitCode', {
    configurable: true, enumerable: true,
    get: () => exitCode,
    set(value) {
      if (value == null) { exitCode = undefined; return; }
      if (typeof value === 'string' && value.trim() !== '') value = Number(value);
      if (typeof value !== 'number')
        throw Object.assign(new TypeError('exitCode must be a number or numeric string'),
            {code: 'ERR_INVALID_ARG_TYPE'});
      if (!Number.isInteger(value))
        throw Object.assign(new RangeError('exitCode must be an integer'), {code: 'ERR_OUT_OF_RANGE'});
      exitCode = value | 0;
    },
  });
  const ioError = () => Object.assign(new Error('Standard stream write made no progress'),
      {code: 'EIO', syscall: 'write'});
  function output(fd) {
    const writable = new stream.Writable({
      autoDestroy: false,
      write(chunk, encoding, done) {
        try {
          // Bound each native call without accumulating or decoding bytes.
          // A blocking OS write completes before this callback succeeds;
          // Writable's own queue provides write(false)/drain semantics.
          let offset = 0;
          while (offset < chunk.byteLength) {
            const part = chunk.subarray(offset, Math.min(offset + 65536, chunk.byteLength));
            const written = call('write', fd, part);
            if (!Number.isInteger(written) || written <= 0 || written > part.byteLength)
              throw ioError();
            offset += written;
          }
          done();
        } catch (failure) { done(failure); }
      },
    });
    Object.assign(writable, {fd, isTTY: Boolean(call('isTTY', fd))});
    return writable;
  }
  const input = new stream.Readable({
    read(size) {
      try {
        const chunk = call('read', 0, Math.min(Math.max(size, 1), 65536));
        if (chunk === null) this.push(null);
        else this.push(Buffer.from(chunk));
      } catch (failure) { this.destroy(failure); }
    },
  });
  Object.assign(input, {fd: 0, isTTY: Boolean(call('isTTY', 0))});
  process.type = 'utility';
  process.execArgv = execArgv.slice();
  process.stdin = input;
  process.stdout = output(1);
  process.stderr = output(2);
  const shutdown = code => {
    if (exiting) return process.exitCode || 0;
    process.exitCode = code === undefined ? process.exitCode || 0 : code;
    exiting = true;
    process._exiting = true;
    process.emit('exit', process.exitCode || 0);
    return process.exitCode || 0;
  };
  process.exit = code => {
    if (exiting) return;
    const result = shutdown(code);
    return exitHost(result);
  };
  return {shutdown};
}

// Electron's parentPort starts paused. Subscribing during an ESM continuation
// must still receive messages posted before that continuation; leave queuing
// and transfer ownership with the native Mojo endpoint.
function installUtilityParentPort({process, EventEmitter, port, setPaused}) {
  class ParentPort extends EventEmitter {
    postMessage(message, transfer) { port.postMessage(message, transfer); }
    start() { setPaused(port, false); }
    pause() { setPaused(port, true); }
  }
  const parent = new ParentPort();
  port.on('message', event => parent.emit('message', event));
  port.on('messageerror', () => parent.emit('messageerror'));
  parent.on('newListener', name => {
    if (name === 'message' && parent.listenerCount('message') === 0) parent.start();
  });
  parent.on('removeListener', name => {
    if (name === 'message' && parent.listenerCount('message') === 0) parent.pause();
  });
  Object.defineProperty(process, 'parentPort', {
    enumerable: true, writable: false, configurable: false, value: parent,
  });
  return parent;
}

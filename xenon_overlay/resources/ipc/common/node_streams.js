// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createNodeStreams({Buffer, EventEmitter, modules, webStreams, getWebStreamState}) {
  const cache = new Map();
  // process is installed after the builtin graph. Resolve its fields when
  // streams run, retaining the same scheduler and standard output identities.
  const processAdapter = {
    nextTick(callback, ...args) {
      if (globalThis.process?.nextTick) globalThis.process.nextTick(callback, ...args);
      else queueMicrotask(() => callback(...args));
    },
    get stdout() { return globalThis.process?.stdout; },
    get stderr() { return globalThis.process?.stderr; },
  };
  const dependencies = {
    'buffer': {Buffer, Blob: globalThis.Blob}, 'safe-buffer': {Buffer},
    'events': EventEmitter, 'process/': processAdapter,
  };
  const entries = {
    'string_decoder/': 'dependencies/string_decoder/lib/string_decoder.js',
    'abort-controller': 'dependencies/abort-controller/dist/abort-controller.js',
    'event-target-shim': 'dependencies/event-target-shim/dist/event-target-shim.js',
  };
  function load(name, parent = '') {
    if (Object.hasOwn(dependencies, name)) return dependencies[name];
    let id = entries[name] || name;
    if (id.startsWith('.')) {
      const parts = parent.split('/').slice(0, -1);
      for (const part of id.split('/')) {
        if (part === '..') parts.pop();
        else if (part && part !== '.') parts.push(part);
      }
      id = parts.join('/');
    }
    if (!modules[id]) id += '.js';
    if (!modules[id]) throw new Error('Missing bundled stream dependency: ' + id);
    if (cache.has(id)) return cache.get(id).exports;
    const module = {exports: {}};
    cache.set(id, module);
    modules[id](child => load(child, id), module, module.exports);
    return module.exports;
  }
  const stream = load('lib/stream.js');
  // The browser build's instanceof test cannot recognize bytes originating
  // in a different V8 realm. Use the intrinsic typed-array brand getter.
  const typedArrayName = Object.getOwnPropertyDescriptor(
    Object.getPrototypeOf(Uint8Array.prototype), Symbol.toStringTag).get;
  stream._isUint8Array = value => ArrayBuffer.isView(value) &&
    typedArrayName.call(value) === 'Uint8Array';
  stream.EventEmitter = EventEmitter;
  stream.default = stream;
  // Modern Node retains the AbortSignal reason on a pipeline AbortError.
  // Keep the upstream state machine and add the missing error metadata only.
  const promisePipeline = stream.promises.pipeline;
  const {isNodeStream, isIterable, isWebStream} = load('lib/internal/streams/utils.js');
  stream.promises.pipeline = async function(...stages) {
    const last = stages[stages.length - 1];
    const options = last && typeof last === 'object' && !isNodeStream(last) &&
      !isIterable(last) && !isWebStream(last) ? last : undefined;
    try { return await promisePipeline(...stages); }
    catch (error) {
      if (error?.code === 'ABORT_ERR' && options?.signal?.aborted &&
          !Object.hasOwn(error, 'cause')) {
        Object.defineProperty(error, 'cause', {value: options.signal.reason,
          configurable: true, writable: true});
      }
      throw error;
    }
  };
  if (Object.getOwnPropertyDescriptor(webStreams, 'ReadableStream')?.get) {
    // Defer the fallback family. Native renderer constructors already exist,
    // so keep their small adapter setup direct instead of adding lazy getters.
    // Later mutations of stream/web or stream exports must not change the
    // intrinsic constructor family used by conversion helpers.
    const adapterDependencies = {Readable: stream.Readable,
      Writable: stream.Writable, Duplex: stream.Duplex, Buffer,
      webStreams: Object.defineProperties({}, {
        ReadableStream: Object.getOwnPropertyDescriptor(webStreams, 'ReadableStream'),
        WritableStream: Object.getOwnPropertyDescriptor(webStreams, 'WritableStream'),
      })};
    let adapters;
    for (const type of ['Readable', 'Writable', 'Duplex']) {
      const constructor = stream[type];
      for (const name of ['fromWeb', 'toWeb']) {
        // Return the real helper on first access, preserving its name, arity and
        // identity without proxies around shared Node stream constructors.
        Object.defineProperty(constructor, name, {enumerable: true, configurable: true,
          get() {
            adapters ??= createStreamWebAdapters(adapterDependencies);
            const value = adapters[type][name];
            if (Object.getOwnPropertyDescriptor(constructor, name)?.configurable)
              Object.defineProperty(constructor, name, {value, enumerable: true,
                writable: true, configurable: true});
            return value;
          },
          set(value) {
            Object.defineProperty(this, name, {value, enumerable: true,
              writable: true, configurable: true});
          },
        });
      }
    }
  } else {
    const adapters = createStreamWebAdapters({Readable: stream.Readable,
      Writable: stream.Writable, Duplex: stream.Duplex, Buffer, webStreams});
    for (const type of ['Readable', 'Writable', 'Duplex']) Object.assign(stream[type], adapters[type]);
  }
  for (const name of ['isDisturbed', 'isErrored', 'isReadable', 'isWritable']) {
    const original = stream[name];
    stream[name] = value => {
      const state = getWebStreamState?.(value);
      if (!state) return original(value);
      return state[{isDisturbed: 'disturbed', isErrored: 'errored',
        isReadable: 'readable', isWritable: 'writable'}[name]];
    };
  }
  return {stream, stringDecoder: load('string_decoder/')};
}

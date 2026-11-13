// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createWebStreamsModule(host, loadPonyfill, setStateReader) {
  const names = [
    'ByteLengthQueuingStrategy', 'CountQueuingStrategy',
    'ReadableByteStreamController', 'ReadableStream', 'ReadableStreamBYOBReader',
    'ReadableStreamBYOBRequest', 'ReadableStreamDefaultController',
    'ReadableStreamDefaultReader', 'TransformStream',
    'TransformStreamDefaultController', 'WritableStream',
    'WritableStreamDefaultController', 'WritableStreamDefaultWriter',
  ];
  // Never mix a ponyfill constructor with Blink's controllers/readers. Their
  // brands differ; browser fetch/Blob must keep the browser's stream identity.
  const native = names.every(name => typeof host[name] === 'function');
  if (!native && typeof ArrayBuffer.prototype.transfer !== 'function' &&
      typeof host.structuredClone !== 'function') {
    // The upstream fallback otherwise returns the input buffer without
    // detaching it. Reporting byte/BYOB support in that state would be false.
    throw Object.assign(new Error('Web byte streams require ArrayBuffer transfer support'),
        {code: 'ERR_NOT_SUPPORTED'});
  }
  let implementation = native ? host : undefined;
  const module = {};
  const lazy = [];
  function initialize() {
    if (implementation) return implementation;
    const loaded = loadPonyfill();
    for (const name of names) {
      if (typeof loaded[name] !== 'function')
        throw new Error('Incomplete Web Streams implementation: ' + name);
    }
    implementation = loaded;
    // Replace only our own accessors. Application assignments/deletions must
    // survive another constructor's first use, like ordinary writable globals.
    for (const {target, name, get, enumerable} of lazy) {
      const descriptor = Object.getOwnPropertyDescriptor(target, name);
      if (descriptor?.get === get && descriptor.configurable)
        Object.defineProperty(target, name, {value: loaded[name], enumerable,
          writable: true, configurable: true});
    }
    lazy.length = 0;
    return loaded;
  }
  function installLazy(target, name, enumerable) {
    const get = () => initialize()[name];
    Object.defineProperty(target, name, {enumerable, configurable: true, get,
      set(value) {
        Object.defineProperty(this, name, {value, enumerable,
          writable: true, configurable: true});
      },
    });
    lazy.push({target, name, get, enumerable});
  }
  for (const name of names) {
    if (native) module[name] = host[name];
    else {
      installLazy(module, name, true);
      installLazy(host, name, false);
    }
  }
  // Encoding and compression transforms are separate platform facilities,
  // outside this ponyfill. Expose them only when real host constructors exist.
  for (const name of ['CompressionStream', 'DecompressionStream',
    'TextDecoderStream', 'TextEncoderStream']) {
    if (typeof host[name] === 'function') module[name] = host[name];
  }
  if (typeof setStateReader === 'function') {
    let readableLocked = native ? Object.getOwnPropertyDescriptor(
        implementation.ReadableStream.prototype, 'locked').get : undefined;
    let writableLocked = native ? Object.getOwnPropertyDescriptor(
        implementation.WritableStream.prototype, 'locked').get : undefined;
    const branded = (getter, value) => {
      try { getter.call(value); return true; } catch (_) { return false; }
    };
    // A private capability for Node stream.isDisturbed/isErrored/isReadable.
    // The fixed upstream implementation owns these actual state fields. Do
    // not publish extra names in stream/web or confuse locked with disturbed.
    setStateReader(value => {
      // No stream from this family can exist before its first constructor is
      // read. Inspecting an ordinary Node stream must not load the ponyfill.
      if (!implementation) return undefined;
      if (!readableLocked) {
        readableLocked = Object.getOwnPropertyDescriptor(
            implementation.ReadableStream.prototype, 'locked').get;
        writableLocked = Object.getOwnPropertyDescriptor(
            implementation.WritableStream.prototype, 'locked').get;
      }
      const readable = branded(readableLocked, value);
      if (!readable && !branded(writableLocked, value)) return undefined;
      if (native) {
        if (typeof host.__xenonGetWebStreamState === 'function')
          return host.__xenonGetWebStreamState(value);
        throw Object.assign(new Error('Native Web Stream state inspection requires a host binding'),
            {code: 'ERR_NOT_SUPPORTED'});
      }
      return {
        kind: readable ? 'readable' : 'writable',
        disturbed: readable && value._disturbed,
        errored: value._state === 'errored',
        readable: readable ? value._state === 'readable' : null,
        writable: readable ? null : value._state === 'writable',
      };
    });
  }
  return module;
}

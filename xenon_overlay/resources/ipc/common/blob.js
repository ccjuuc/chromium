// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Memory-backed Blob for the plain V8 main context. Renderers keep Blink's
// native Blob. Private immutable chunks allow nested Blobs and slices to share
// storage, while every exposed buffer is a copy.
function createBlobConstructor({platform, ReadableStream, getReadableStream = () => ReadableStream}) {
  const encode = value => {
    const bytes = new Uint8Array(value.length * 3);
    let length = 0;
    for (let i = 0; i < value.length; ++i) {
      let point = value.charCodeAt(i);
      if (point >= 0xd800 && point <= 0xdbff) {
        const next = value.charCodeAt(i + 1);
        if (next >= 0xdc00 && next <= 0xdfff) {
          point = 0x10000 + ((point - 0xd800) << 10) + next - 0xdc00;
          ++i;
        } else point = 0xfffd;
      } else if (point >= 0xdc00 && point <= 0xdfff) point = 0xfffd;
      if (point < 0x80) bytes[length++] = point;
      else if (point < 0x800) {
        bytes[length++] = 0xc0 | (point >> 6);
        bytes[length++] = 0x80 | (point & 0x3f);
      } else if (point < 0x10000) {
        bytes[length++] = 0xe0 | (point >> 12);
        bytes[length++] = 0x80 | ((point >> 6) & 0x3f);
        bytes[length++] = 0x80 | (point & 0x3f);
      } else {
        bytes[length++] = 0xf0 | (point >> 18);
        bytes[length++] = 0x80 | ((point >> 12) & 0x3f);
        bytes[length++] = 0x80 | ((point >> 6) & 0x3f);
        bytes[length++] = 0x80 | (point & 0x3f);
      }
    }
    return bytes.slice(0, length);
  };
  // UTF-8 replacement decoding, including incomplete sequences, invalid
  // continuations, overlong forms, surrogate encodings, and a leading BOM.
  const decode = bytes => {
    const pieces = [];
    let text = '';
    let first = true;
    const append = point => {
      if (!(first && point === 0xfeff)) text += String.fromCodePoint(point);
      first = false;
      if (text.length >= 8192) { pieces.push(text); text = ''; }
    };
    for (let i = 0; i < bytes.length;) {
      const lead = bytes[i++];
      if (lead < 0x80) { append(lead); continue; }
      const count = lead >= 0xc2 && lead <= 0xdf ? 1 :
          lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
      if (!count) { append(0xfffd); continue; }
      let point = lead & (0x7f >> count);
      let valid = true;
      for (let j = 0; j < count; ++j) {
        const byte = bytes[i];
        const low = j === 0 && lead === 0xe0 ? 0xa0 :
            j === 0 && lead === 0xf0 ? 0x90 : 0x80;
        const high = j === 0 && lead === 0xed ? 0x9f :
            j === 0 && lead === 0xf4 ? 0x8f : 0xbf;
        if (i >= bytes.length || byte < low || byte > high) { valid = false; break; }
        point = (point << 6) | (byte & 0x3f);
        ++i;
      }
      append(valid ? point : 0xfffd);
    }
    pieces.push(text);
    return pieces.join('');
  };
  const states = new WeakMap();
  const invalid = message => Object.assign(new TypeError(message),
      {code: 'ERR_INVALID_ARG_TYPE'});
  const stateOf = value => {
    const state = states.get(value);
    if (!state) throw Object.assign(new TypeError('Value of this must be of type Blob'),
        {code: 'ERR_INVALID_THIS'});
    return state;
  };
  const bufferLength = Object.getOwnPropertyDescriptor(
      ArrayBuffer.prototype, 'byteLength').get;
  const sharedLength = typeof SharedArrayBuffer === 'function' ?
      Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, 'byteLength').get : null;
  const isBuffer = value => {
    try { bufferLength.call(value); return true; } catch (_) {}
    if (sharedLength) {
      try { sharedLength.call(value); return true; } catch (_) {}
    }
    return false;
  };
  const toString = value => `${value}`;
  const typeOf = value => {
    const type = toString(value);
    return /[^\x20-\x7e]/.test(type) ? '' : type.toLowerCase();
  };
  // WebIDL signed long long conversion, including wraparound and infinities.
  const integer = value => {
    const number = +value;
    if (!Number.isFinite(number) || number === 0) return 0;
    return Number(BigInt.asIntN(64, BigInt(Math.trunc(number))));
  };
  const materialize = state => {
    const bytes = new Uint8Array(state.size);
    let offset = 0;
    for (const chunk of state.chunks) {
      bytes.set(chunk, offset);
      offset += chunk.length;
    }
    return bytes;
  };
  class Blob {
    constructor(parts = [], options) {
      if (parts === null || (typeof parts !== 'object' && typeof parts !== 'function'))
        throw invalid('Value cannot be converted to sequence');
      const iterator = parts[Symbol.iterator];
      if (typeof iterator !== 'function') throw invalid('Value cannot be converted to sequence');
      // Convert the sequence before reading options, matching Node's ordering.
      const sources = Array.from({[Symbol.iterator]: () => Reflect.apply(iterator, parts, [])}, part => {
        if (typeof part === 'symbol') throw invalid('Cannot convert a Symbol to a string');
        return states.has(part) || isBuffer(part) || ArrayBuffer.isView(part) ? part : toString(part);
      });
      if (options != null && typeof options !== 'object' && typeof options !== 'function')
        throw invalid('The options argument must be an object');
      let {endings = 'transparent', type = ''} = options ?? {};
      endings = toString(endings);
      if (endings !== 'transparent' && endings !== 'native')
        throw Object.assign(new TypeError('Invalid options.endings'), {code: 'ERR_INVALID_ARG_VALUE'});
      const chunks = [];
      let size = 0;
      for (const part of sources) {
        if (states.has(part)) {
          const state = states.get(part);
          for (const chunk of state.chunks) chunks.push(chunk);
          size += state.size;
          continue;
        }
        let chunk;
        if (ArrayBuffer.isView(part)) {
          chunk = new Uint8Array(new Uint8Array(part.buffer, part.byteOffset, part.byteLength));
        } else if (isBuffer(part)) {
          chunk = new Uint8Array(new Uint8Array(part));
        } else {
          const text = endings === 'native' ?
              part.replace(/\r\n|\n/g, platform === 'win32' ? '\r\n' : '\n') : part;
          chunk = encode(text);
        }
        if (chunk.length) chunks.push(chunk);
        size += chunk.length;
      }
      states.set(this, {chunks, size, type: typeOf(type)});
    }
    get size() { return stateOf(this).size; }
    get type() { return stateOf(this).type; }
    slice(start = 0, end = undefined, contentType = '') {
      const state = stateOf(this);
      const normalize = value => value < 0 ? Math.max(state.size + value, 0) :
          Math.min(value, state.size);
      start = normalize(integer(start));
      end = normalize(end === undefined ? state.size : integer(end));
      const size = Math.max(end - start, 0);
      const chunks = [];
      let offset = 0;
      for (const chunk of state.chunks) {
        const from = Math.max(start - offset, 0);
        const to = Math.min(end - offset, chunk.length);
        if (from < to) chunks.push(chunk.subarray(from, to));
        offset += chunk.length;
        if (offset >= end) break;
      }
      const result = Object.create(Blob.prototype);
      states.set(result, {chunks, size, type: typeOf(contentType)});
      return result;
    }
    async arrayBuffer() { return materialize(stateOf(this)).buffer; }
    async bytes() { return materialize(stateOf(this)); }
    async text() { return decode(materialize(stateOf(this))); }
    stream() {
      let chunks = stateOf(this).chunks.slice();
      const ReadableStream = getReadableStream();
      if (typeof ReadableStream !== 'function')
        throw Object.assign(new Error('Blob.stream requires Web Streams support'),
            {code: 'ERR_NOT_SUPPORTED'});
      let index = 0, offset = 0;
      return new ReadableStream({
        type: 'bytes',
        pull(controller) {
          if (index === chunks.length) {
            controller.close();
            controller.byobRequest?.respond(0);
            return;
          }
          const chunk = chunks[index];
          const end = Math.min(offset + 65536, chunk.length);
          // enqueue transfers its input buffer. Never expose or detach the
          // Blob's immutable backing, including shared slices/nested Blobs.
          const copy = chunk.slice(offset, end);
          offset = end;
          if (offset === chunk.length) { ++index; offset = 0; }
          controller.enqueue(copy);
          if (index === chunks.length) { chunks = []; index = 0; controller.close(); }
        },
        cancel() { chunks = []; },
      });
    }
  }
  Object.defineProperty(Blob.prototype, Symbol.toStringTag, {value: 'Blob', configurable: true});
  for (const name of ['size', 'type', 'slice', 'arrayBuffer', 'bytes', 'text', 'stream'])
    Object.defineProperty(Blob.prototype, name, {enumerable: true});
  return Blob;
}

// File is immutable Blob content plus metadata. This does not open a path or
// grant filesystem access. Renderers retain Blink's File constructor.
function createFileConstructor({Blob}) {
  const states = new WeakMap();
  const stateOf = value => {
    const state = states.get(value);
    if (!state) throw Object.assign(new TypeError('Value of this must be of type File'),
        {code: 'ERR_INVALID_THIS'});
    return state;
  };
  const toUSVString = value => {
    const text = `${value}`;
    if (typeof String.prototype.toWellFormed === 'function')
      return String.prototype.toWellFormed.call(text);
    let result = '';
    for (let index = 0; index < text.length; ++index) {
      const code = text.charCodeAt(index);
      if (code >= 0xd800 && code <= 0xdbff) {
        const next = text.charCodeAt(index + 1);
        if (next >= 0xdc00 && next <= 0xdfff) result += text[index] + text[++index];
        else result += '\ufffd';
      } else result += code >= 0xdc00 && code <= 0xdfff ? '\ufffd' : text[index];
    }
    return result;
  };
  class File extends Blob {
    constructor(fileBits, fileName, options = {}) {
      if (arguments.length < 2)
        throw Object.assign(new TypeError('The fileBits and fileName arguments must be specified'),
            {code: 'ERR_MISSING_ARGS'});
      // Preserve Node's observable sequence/getter ordering and Blob copies.
      super(fileBits, options);
      let {lastModified} = options ?? {};
      if (lastModified === undefined) lastModified = Date.now();
      else {
        // ToNumber rejects bigint; Node preserves finite fractions/infinities
        // here and normalizes NaN, unlike a WebIDL long-long conversion.
        lastModified = +lastModified;
        if (Number.isNaN(lastModified)) lastModified = 0;
      }
      states.set(this, {name: toUSVString(fileName), lastModified});
    }
    get name() { return stateOf(this).name; }
    get lastModified() { return stateOf(this).lastModified; }
  }
  Object.defineProperty(File.prototype, Symbol.toStringTag, {value: 'File', configurable: true});
  for (const name of ['name', 'lastModified'])
    Object.defineProperty(File.prototype, name, {enumerable: true});
  return File;
}

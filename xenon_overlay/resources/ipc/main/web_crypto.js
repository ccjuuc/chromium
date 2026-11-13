  function createMainWebCrypto({randomBytes, randomUUID, digest, DOMException}) {
    const cryptoBrand = new WeakSet();
    const subtleBrand = new WeakSet();
    const typedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype);
    const typedName = Object.getOwnPropertyDescriptor(typedArrayPrototype, Symbol.toStringTag).get;
    const typedBuffer = Object.getOwnPropertyDescriptor(typedArrayPrototype, 'buffer').get;
    const typedOffset = Object.getOwnPropertyDescriptor(typedArrayPrototype, 'byteOffset').get;
    const typedLength = Object.getOwnPropertyDescriptor(typedArrayPrototype, 'byteLength').get;
    const integers = new Set(['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array',
      'Uint16Array', 'Int32Array', 'Uint32Array', 'BigInt64Array', 'BigUint64Array']);
    function brand(set, receiver) {
      if (!set.has(receiver)) throw Object.assign(new TypeError('Invalid crypto receiver'), {code: 'ERR_INVALID_THIS'});
    }
    function bufferLength(buffer) {
      return Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get.call(buffer);
    }
    function sourceBytes(data) {
      if (ArrayBuffer.isView(data)) {
        const dataView = typedName.call(data) === undefined;
        const buffer = dataView ?
          Object.getOwnPropertyDescriptor(DataView.prototype, 'buffer').get.call(data) : typedBuffer.call(data);
        bufferLength(buffer); // SharedArrayBuffer is not a WebCrypto BufferSource.
        const offset = dataView ?
          Object.getOwnPropertyDescriptor(DataView.prototype, 'byteOffset').get.call(data) : typedOffset.call(data);
        const size = dataView ?
          Object.getOwnPropertyDescriptor(DataView.prototype, 'byteLength').get.call(data) : typedLength.call(data);
        return new Uint8Array(new Uint8Array(buffer, offset, size));
      }
      return new Uint8Array(new Uint8Array(data, 0, bufferLength(data)));
    }
    class SubtleCrypto {
      constructor() { throw Object.assign(new TypeError('Illegal constructor'), {code: 'ERR_ILLEGAL_CONSTRUCTOR'}); }
      async digest(algorithm, data) {
        brand(subtleBrand, this);
        const name = typeof algorithm === 'string' ? algorithm : algorithm?.name;
        if (typeof name !== 'string') throw new TypeError('Algorithm requires a name');
        if (!/^SHA-(1|256|384|512)$/i.test(name))
          throw new DOMException('Unrecognized algorithm name', 'NotSupportedError');
        const input = sourceBytes(data);
        const result = digest(name.toLowerCase().replace('-', ''), input);
        return Uint8Array.from(result).buffer;
      }
    }
    for (const name of ['encrypt', 'decrypt', 'sign', 'verify', 'generateKey', 'deriveKey',
      'deriveBits', 'importKey', 'exportKey', 'wrapKey', 'unwrapKey']) {
      Object.defineProperty(SubtleCrypto.prototype, name, {enumerable: true, configurable: true,
        writable: true, value: async function() {
          brand(subtleBrand, this);
          throw new DOMException('subtle.' + name + ' is not supported', 'NotSupportedError');
        }});
    }
    const subtle = Object.create(SubtleCrypto.prototype);
    subtleBrand.add(subtle);
    class Crypto {
      constructor() { throw Object.assign(new TypeError('Illegal constructor'), {code: 'ERR_ILLEGAL_CONSTRUCTOR'}); }
      get subtle() { brand(cryptoBrand, this); return subtle; }
      getRandomValues(array) {
        brand(cryptoBrand, this);
        if (!ArrayBuffer.isView(array) || !integers.has(typedName.call(array)))
          throw new DOMException('Expected an integer TypedArray', 'TypeMismatchError');
        const size = typedLength.call(array);
        if (size > 65536) throw new DOMException('Requested length exceeds 65536 bytes', 'QuotaExceededError');
        const target = new Uint8Array(typedBuffer.call(array), typedOffset.call(array), size);
        target.set(randomBytes(size));
        return array;
      }
      randomUUID() { brand(cryptoBrand, this); return randomUUID(); }
    }
    for (const [prototype, name] of [[Crypto.prototype, 'Crypto'], [SubtleCrypto.prototype, 'SubtleCrypto']]) {
      Object.defineProperty(prototype, Symbol.toStringTag, {value: name, configurable: true});
      for (const key of Object.getOwnPropertyNames(prototype)) {
        if (key !== 'constructor') Object.defineProperty(prototype, key,
          {...Object.getOwnPropertyDescriptor(prototype, key), enumerable: true});
      }
    }
    const crypto = Object.create(Crypto.prototype);
    cryptoBrand.add(crypto);
    return {crypto, Crypto, SubtleCrypto};
  }

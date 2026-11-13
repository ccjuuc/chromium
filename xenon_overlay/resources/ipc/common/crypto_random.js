// Shared Node random-fill view and range validation. Typed-array offsets
// count elements; DataView and ArrayBuffer offsets count bytes.
  function randomArgumentError(name, value, range) {
    const error = typeof value === 'number' ?
        new RangeError(`${name} is outside ${range}`) :
        new TypeError(`${name} must be a number`);
    error.code = typeof value === 'number' ? 'ERR_OUT_OF_RANGE' : 'ERR_INVALID_ARG_TYPE';
    return error;
  }
  function randomByteCount(value, name, elementSize, maximum) {
    const bytes = typeof value === 'number' ? value * elementSize : NaN;
    if (typeof value !== 'number' || !Number.isFinite(bytes) || bytes < 0 ||
        bytes > Math.min(maximum, 0x7fffffff)) {
      throw randomArgumentError(name, value, `0..${Math.min(maximum, 0x7fffffff)}`);
    }
    return Math.floor(bytes);
  }
  function randomTargetBytes(buf, offset = 0, size) {
    const view = ArrayBuffer.isView(buf);
    const buffer = view ? buf.buffer : buf;
    if (!view && !(buffer instanceof ArrayBuffer) &&
        !(typeof SharedArrayBuffer === 'function' && buffer instanceof SharedArrayBuffer)) {
      const error = new TypeError('buf must be an ArrayBuffer or an ArrayBuffer view');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
    const elementSize = buf.BYTES_PER_ELEMENT || 1;
    const byteOffset = randomByteCount(offset, 'offset', elementSize, buf.byteLength);
    const byteLength = size === undefined ? buf.byteLength - byteOffset :
        randomByteCount(size, 'size', elementSize, buf.byteLength - byteOffset);
    return new Uint8Array(buffer, (view ? buf.byteOffset : 0) + byteOffset, byteLength);
  }

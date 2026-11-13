// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createNativeImageApi({callSync, readFileSync}) {
  const maxEncodedBytes = 32 * 1024 * 1024;
  const images = new WeakMap();
  const invalid = message => Object.assign(new TypeError(message),
      {code: 'ERR_INVALID_ARG_TYPE'});
  const unsupported = feature => {
    throw Object.assign(new Error('nativeImage does not support ' + feature),
        {code: 'ERR_NOT_SUPPORTED'});
  };
  function scaleFactor(scale) {
    if (scale === undefined || scale === 1) return;
    if (typeof scale !== 'number' || !Number.isFinite(scale) || scale <= 0)
      throw invalid('scaleFactor must be a positive number');
    unsupported('multiple scale representations');
  }
  function options(value) {
    if (value === undefined) return;
    if (!value || typeof value !== 'object' || Array.isArray(value))
      throw invalid('NativeImage options must be an object');
    for (const key of Object.keys(value)) {
      if (key !== 'scaleFactor') unsupported('option ' + key);
    }
    scaleFactor(value.scaleFactor);
  }
  function stateOf(image) {
    const state = images.get(image);
    if (!state) throw invalid('Expected a NativeImage');
    return state;
  }
  function bufferInput(buffer) {
    if (!Buffer.isBuffer(buffer)) throw invalid('buffer must be a Buffer');
    if (buffer.length > maxEncodedBytes)
      throw Object.assign(new RangeError('encoded image exceeds 32 MiB'),
          {code: 'ERR_BUFFER_TOO_LARGE'});
    return {dataBase64: buffer.toString('base64')};
  }
  function make(state, path) {
    if (!state || typeof state.pngBase64 !== 'string' ||
        !Number.isInteger(state.width) || !Number.isInteger(state.height) ||
        state.width < 0 || state.height < 0 ||
        (!state.pngBase64 !== (state.width === 0 && state.height === 0)) ||
        (state.pngBase64 && (!state.width || !state.height))) {
      throw Object.assign(new Error('Invalid native image response'),
          {code: 'ERR_OPERATION_FAILED'});
    }
    const image = Object.create(NativeImage.prototype);
    images.set(image, {pngBase64: state.pngBase64, width: state.width, height: state.height});
    // Existing native menus serialize file-backed icons by path. Keep only a
    // successfully decoded path, and prevent later mutation of its identity.
    if (path && state.pngBase64)
      Object.defineProperty(image, 'path', {value: path, enumerable: true});
    return image;
  }
  function empty() { return make({pngBase64: '', width: 0, height: 0}); }
  class NativeImage {
    isEmpty() { return !stateOf(this).pngBase64; }
    getSize(scale) {
      scaleFactor(scale);
      const {width, height} = stateOf(this);
      return {width, height};
    }
    getAspectRatio(scale) {
      const {width, height} = this.getSize(scale);
      return height ? width / height : 1;
    }
    getScaleFactors() { return this.isEmpty() ? [] : [1]; }
    toPNG(value) { options(value); return Buffer.from(stateOf(this).pngBase64, 'base64'); }
    toDataURL(value) { options(value); return 'data:image/png;base64,' + stateOf(this).pngBase64; }
    toJPEG(quality) {
      if (!Number.isInteger(quality) || quality < 0 || quality > 100)
        throw Object.assign(new RangeError('JPEG quality must be an integer from 0 to 100'),
            {code: 'ERR_OUT_OF_RANGE'});
      const {pngBase64} = stateOf(this);
      return pngBase64 ? Buffer.from(callSync('nativeImage.toJPEG', {pngBase64, quality}), 'base64') : Buffer.alloc(0);
    }
    toBitmap(value) {
      options(value);
      const {pngBase64} = stateOf(this);
      return pngBase64 ? Buffer.from(callSync('nativeImage.toBitmap', {pngBase64}), 'base64') : Buffer.alloc(0);
    }
    getBitmap(value) { return this.toBitmap(value); }
    resize() { return unsupported('resize'); }
    crop() { return unsupported('crop'); }
    addRepresentation() { return unsupported('multiple representations'); }
    getNativeHandle() { return unsupported('native handles'); }
    setTemplateImage() { return unsupported('template images'); }
    isTemplateImage() { return unsupported('template images'); }
    get [Symbol.toStringTag]() { return 'NativeImage'; }
  }
  return {
    createEmpty: empty,
    createFromBuffer(buffer, value) {
      if (!Buffer.isBuffer(buffer)) throw invalid('buffer must be a Buffer');
      options(value);
      if (!buffer.length) return empty();
      return make(callSync('nativeImage.decode', bufferInput(buffer)));
    },
    createFromDataURL(dataURL) {
      if (typeof dataURL !== 'string') throw invalid('dataURL must be a string');
      if (dataURL.length > maxEncodedBytes * 4)
        throw Object.assign(new RangeError('image data URL exceeds limit'),
            {code: 'ERR_BUFFER_TOO_LARGE'});
      return make(callSync('nativeImage.decode', {dataURL}));
    },
    createFromPath(path) {
      if (typeof path !== 'string') throw invalid('path must be a string');
      if (!path || path.includes('\0')) return empty();
      let bytes;
      try { bytes = readFileSync(path); }
      catch (error) {
        if (['ENOENT', 'ENOTDIR', 'EISDIR', 'EACCES', 'EPERM', 'EINVAL'].includes(error?.code)) return empty();
        throw error;
      }
      return make(callSync('nativeImage.decode', bufferInput(bytes)), path);
    },
    createFromBitmap: () => unsupported('raw bitmap input'),
    createFromNamedImage: () => unsupported('platform named images'),
    createThumbnailFromPath: async () => unsupported('thumbnail generation'),
  };
}

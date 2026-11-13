// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createSafeStorageApi({Buffer, platform, isReady, callHost, callHostAsync}) {
  const invalid = message => Object.assign(new TypeError(message),
    {code: 'ERR_INVALID_ARG_TYPE'});
  function checkReady(async) {
    // Legacy synchronous Electron APIs can be used before ready on Windows
    // and macOS once their native encryptor is initialized. Modern async APIs
    // require app readiness on every platform.
    if ((async || platform === 'linux') && !isReady())
      throw Object.assign(new Error('safeStorage cannot be used before app is ready'),
        {code: 'ERR_NOT_READY'});
  }
  function checkString(text) {
    if (typeof text !== 'string') throw invalid('Plaintext must be a string');
    return text;
  }
  function encodeBuffer(buffer) {
    if (!Buffer.isBuffer(buffer)) throw invalid('Ciphertext must be a Buffer');
    return buffer.toString('base64');
  }
  const api = {
    isEncryptionAvailable() {
      if (platform === 'linux' && !isReady()) return false;
      return callHost({method: 'isEncryptionAvailable'});
    },
    encryptString(text) {
      checkReady(false);
      return Buffer.from(callHost({method: 'encryptString', text: checkString(text)}),
        'base64');
    },
    decryptString(buffer) {
      checkReady(false);
      return callHost({method: 'decryptString', ciphertext: encodeBuffer(buffer)});
    },
    setUsePlainTextEncryption(allow) {
      if (typeof allow !== 'boolean')
        throw invalid('setUsePlainTextEncryption expects a boolean');
      callHost({method: 'setUsePlainTextEncryption', allow});
    },
    async isAsyncEncryptionAvailable() {
      if (!isReady()) return false;
      return callHostAsync({method: 'isAsyncEncryptionAvailable'});
    },
    async encryptStringAsync(text) {
      checkReady(true);
      const ciphertext = await callHostAsync({method: 'encryptStringAsync',
        text: checkString(text)});
      return Buffer.from(ciphertext, 'base64');
    },
    async decryptStringAsync(buffer) {
      checkReady(true);
      return callHostAsync({method: 'decryptStringAsync', ciphertext: encodeBuffer(buffer)});
    },
  };
  if (platform === 'linux') {
    api.getSelectedStorageBackend = () => isReady()
      ? callHost({method: 'getSelectedStorageBackend'}) : 'unknown';
  }
  return api;
}

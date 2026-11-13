// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createNodePromisify() {
  const custom = Symbol.for('nodejs.util.promisify.custom');
  const invalid = name => Object.assign(
      new TypeError(`The "${name}" argument must be a function`),
      {code: 'ERR_INVALID_ARG_TYPE'});
  function promisify(original) {
    if (typeof original !== 'function') throw invalid('original');
    if (original[custom]) {
      const fn = original[custom];
      if (typeof fn !== 'function') throw invalid('util.promisify.custom');
      Object.defineProperty(fn, custom,
          {value: fn, configurable: true, enumerable: false, writable: false});
      return fn;
    }
    function fn(...args) {
      return new Promise((resolve, reject) => {
        Reflect.apply(original, this,
            [...args, (error, value) => error ? reject(error) : resolve(value)]);
      });
    }
    Object.setPrototypeOf(fn, Object.getPrototypeOf(original));
    Object.defineProperty(fn, custom,
        {value: fn, configurable: true, enumerable: false, writable: false});
    Object.defineProperties(fn, Object.getOwnPropertyDescriptors(original));
    return fn;
  }
  promisify.custom = custom;
  return promisify;
}

// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function processEnvironmentSnapshot(source, caseInsensitive) {
  if (!caseInsensitive) return source;
  const names = new Map(Object.keys(source).map(name => [name.toUpperCase(), name]));
  const canonical = key => typeof key === 'string' ? names.get(key.toUpperCase()) ?? key : key;
  const remember = key => { if (typeof key === 'string') names.set(key.toUpperCase(), key); };
  return new Proxy(source, {
    get: (target, key) => Reflect.get(target, canonical(key)),
    has: (target, key) => Reflect.has(target, canonical(key)),
    getOwnPropertyDescriptor: (target, key) => Reflect.getOwnPropertyDescriptor(target, canonical(key)),
    set(target, key, value) {
      key = canonical(key);
      const success = Reflect.defineProperty(target, key,
        {value, writable: true, enumerable: true, configurable: true});
      if (success) remember(key);
      return success;
    },
    deleteProperty(target, key) {
      key = canonical(key);
      const success = Reflect.deleteProperty(target, key);
      if (success && typeof key === 'string') names.delete(key.toUpperCase());
      return success;
    },
    defineProperty(target, key, descriptor) {
      if (!Object.hasOwn(descriptor, 'value') || !descriptor.writable ||
          !descriptor.enumerable || !descriptor.configurable) {
        throw Object.assign(new TypeError('process.env requires a configurable, writable, enumerable data descriptor'),
          {code: 'ERR_INVALID_OBJECT_DEFINE_PROPERTY'});
      }
      key = canonical(key);
      const success = Reflect.defineProperty(target, key, descriptor);
      if (success) remember(key);
      return success;
    },
    preventExtensions: () => false,
  });
}

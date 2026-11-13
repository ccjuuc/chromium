// Strict comparison logic adapted from Node.js v24.19.0,
// lib/internal/util/comparisons.js. Host-independent brand and byte helpers
// replace Node's internal bindings; loose and partial comparison are omitted.
//
// Copyright Node.js contributors. All rights reserved.
// Copyright Joyent, Inc. and other Node contributors. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

function createIsDeepStrictEqual(types = {}) {
  const uncurry = fn => Function.prototype.call.bind(fn);
  const own = uncurry(Object.prototype.hasOwnProperty);
  const enumerable = uncurry(Object.prototype.propertyIsEnumerable);
  const toString = uncurry(Object.prototype.toString);
  const objectIs = Object.is;
  const keys = Object.keys;
  const symbols = Object.getOwnPropertySymbols;
  const descriptor = Object.getOwnPropertyDescriptor;
  const prototype = Object.getPrototypeOf;
  const arrayIsArray = Array.isArray;
  const isView = ArrayBuffer.isView;
  const toStringTag = Symbol.toStringTag;
  const Uint8 = Uint8Array;
  const SafeSet = Set;
  const ErrorConstructor = Error;
  const PromiseConstructor = Promise;
  const typedPrototype = prototype(Uint8.prototype);
  const getter = (target, name) => uncurry(descriptor(target, name).get);
  const typedTag = getter(typedPrototype, toStringTag);
  const typedBuffer = getter(typedPrototype, 'buffer');
  const typedOffset = getter(typedPrototype, 'byteOffset');
  const typedLength = getter(typedPrototype, 'byteLength');
  const viewBuffer = getter(DataView.prototype, 'buffer');
  const viewOffset = getter(DataView.prototype, 'byteOffset');
  const viewLength = getter(DataView.prototype, 'byteLength');
  const bufferLength = getter(ArrayBuffer.prototype, 'byteLength');
  const sharedLength = typeof SharedArrayBuffer === 'function' ?
      getter(SharedArrayBuffer.prototype, 'byteLength') : null;
  const mapSize = getter(Map.prototype, 'size');
  const setSize = getter(Set.prototype, 'size');
  const dateTime = uncurry(Date.prototype.getTime);
  const regexpSource = getter(RegExp.prototype, 'source');
  const weakMapHas = uncurry(WeakMap.prototype.has);
  const weakSetHas = uncurry(WeakSet.prototype.has);
  const noBrand = Symbol('not a branded object');
  const brand = (operation, value) => {
    try { return operation(value); } catch { return noBrand; }
  };
  const isMap = value => brand(mapSize, value) !== noBrand;
  const isSet = value => brand(setSize, value) !== noBrand;
  const isDate = value => brand(dateTime, value) !== noBrand;
  const isRegExp = value => brand(regexpSource, value) !== noBrand;
  const isBuffer = value => brand(bufferLength, value) !== noBrand ||
      (sharedLength !== null && brand(sharedLength, value) !== noBrand);
  const isWeak = value => brand(v => weakMapHas(v, noBrand), value) !== noBrand ||
      brand(v => weakSetHas(v, noBrand), value) !== noBrand;
  const nativeError = types.isNativeError || Error.isError ||
      (value => value instanceof ErrorConstructor);
  const isError = value => nativeError(value) || value instanceof ErrorConstructor;
  // No standard JS operation can inspect a Promise's internal slot without
  // adding reactions. Hosts may supply V8's IsPromise predicate to distinguish
  // cross-realm promises from objects deliberately given Promise.prototype.
  const isPromise = types.isPromise || (value => {
    if (value instanceof PromiseConstructor) return true;
    const parent = prototype(value);
    return parent !== null && own(parent, toStringTag) &&
        parent[toStringTag] === 'Promise' &&
        typeof parent.then === 'function' && typeof parent.catch === 'function';
  });
  const boxed = [Number, String, Boolean, BigInt, Symbol].map(constructor =>
      uncurry(constructor.prototype.valueOf));
  const boxedValue = value => {
    for (let index = 0; index < boxed.length; ++index) {
      const result = brand(boxed[index], value);
      if (result !== noBrand) return {index, value: result};
    }
    return null;
  };
  const urlHref = typeof URL === 'function' && descriptor(URL.prototype, 'href')?.get ?
      getter(URL.prototype, 'href') : null;
  const cryptoKeyType = typeof CryptoKey === 'function' && descriptor(CryptoKey.prototype, 'type')?.get ?
      getter(CryptoKey.prototype, 'type') : null;
  const wellKnown = new SafeSet([
    Array, ArrayBuffer, BigInt, BigInt64Array, BigUint64Array, Boolean,
    DataView, Date, Error, Float32Array, Float64Array, Function,
    Int16Array, Int32Array, Int8Array, Map, Number, Object, Promise,
    RegExp, Set, String, Symbol, Uint16Array, Uint32Array, Uint8Array,
    Uint8ClampedArray, WeakMap, WeakSet,
  ]);
  if (typeof Buffer === 'function') wellKnown.add(Buffer);
  if (typeof Float16Array === 'function') wellKnown.add(Float16Array);

  const unequalTag = (a, b) => a[toStringTag] !== b[toStringTag];
  const unequalSlowTag = (tag, a, b) =>
    a[toStringTag] !== undefined && b[toStringTag] !== undefined ?
      unequalTag(a, b) : tag !== toString(b);
  const nonIndexKeys = value => {
    const result = keys(value).filter(key => {
      const number = Number(key);
      return !(number >= 0 && number < 0xffffffff &&
          Math.floor(number) === number && String(number) === key);
    });
    for (const key of symbols(value)) if (enumerable(value, key)) result.push(key);
    return result;
  };
  const viewBytes = value => {
    if (typedTag(value) === undefined) {
      return new Uint8(viewBuffer(value), viewOffset(value), viewLength(value));
    }
    // Detached and out-of-bounds typed arrays have zero bytes. Unlike creating
    // a new view over their detached buffer, Node's native byte compare works.
    const length = typedLength(value);
    return length === 0 ? new Uint8(0) :
        new Uint8(typedBuffer(value), typedOffset(value), length);
  };
  const equalBytes = (a, b) => {
    if (a.length !== b.length) return false;
    for (let index = 0; index < a.length; ++index) if (a[index] !== b[index]) return false;
    return true;
  };

  function inner(a, b, skipPrototype, memos) {
    if (a === b) return a !== 0 || objectIs(a, b);
    if (typeof a === 'number') return a !== a && b !== b;
    if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) return false;
    return compareObject(a, b, skipPrototype, memos);
  }

  function compareObject(a, b, skipPrototype, memos) {
    if (!skipPrototype) {
      // Match Node 24's constructor shortcut, including user prototypes whose
      // inherited constructor is identical and objects with own constructors.
      if (wellKnown.has(a.constructor) || (a.constructor !== undefined && !own(a, 'constructor'))) {
        if (a.constructor !== b.constructor) return false;
      } else if (prototype(a) !== prototype(b)) return false;
    }
    if (arrayIsArray(a)) {
      if (!arrayIsArray(b) || a.length !== b.length || unequalTag(a, b)) return false;
      const list = nonIndexKeys(b);
      if (list.length !== nonIndexKeys(a).length) return false;
      return keyCheck(a, b, skipPrototype, memos, 1, list);
    }
    let tag;
    if (a[toStringTag] === undefined && (tag = toString(a)) === '[object Object]') {
      if (unequalSlowTag(tag, a, b)) return false;
      return keyCheck(a, b, skipPrototype, memos, 0);
    }
    if (isSet(a)) {
      if (!isSet(b) || a.size !== b.size || unequalTag(a, b)) return false;
      return keyCheck(a, b, skipPrototype, memos, 2);
    }
    if (isMap(a)) {
      if (!isMap(b) || a.size !== b.size || unequalTag(a, b)) return false;
      return keyCheck(a, b, skipPrototype, memos, 3);
    }
    if (isView(a)) {
      if (typedTag(a) !== typedTag(b) || a.byteLength !== b.byteLength) return false;
      // With skipPrototype, Node's byte comparator reports an invalid second
      // argument for a DataView versus a non-view with the same byteLength.
      if (!isView(b)) {
        throw Object.assign(new TypeError('The second byte comparison argument must be an ArrayBufferView'),
            {code: 'ERR_INVALID_ARG_TYPE'});
      }
      if (!equalBytes(viewBytes(a), viewBytes(b))) return false;
      const list = nonIndexKeys(b);
      if (list.length !== nonIndexKeys(a).length) return false;
      return keyCheck(a, b, skipPrototype, memos, 0, list);
    }
    if (isDate(a)) {
      if (!isDate(b) || unequalTag(a, b) || !objectIs(dateTime(a), dateTime(b))) return false;
    } else if (isRegExp(a)) {
      if (!isRegExp(b) || a.source !== b.source || a.flags !== b.flags ||
          a.lastIndex !== b.lastIndex || unequalTag(a, b)) return false;
    } else if (isBuffer(a)) {
      if (!isBuffer(b) || unequalTag(a, b) || a.byteLength !== b.byteLength ||
          !equalBytes(new Uint8(a), new Uint8(b))) return false;
    } else if (unequalSlowTag(tag ?? toString(a), a, b) || arrayIsArray(b) ||
        isView(b) || isSet(b) || isMap(b) || isDate(b) || isRegExp(b) || isBuffer(b)) {
      return false;
    } else if (isError(a)) {
      if (!isError(b)) return false;
      // The nonenumerable stack is diagnostic only. Error message/name/cause
      // and AggregateError.errors are semantic even when nonenumerable.
      for (const key of ['message', 'name', 'cause', 'errors']) {
        if (!enumerable(b, key) && !inner(a[key], b[key], skipPrototype, memos)) return false;
      }
      if (own(a, 'cause') !== own(b, 'cause')) return false;
    } else {
      const boxA = boxedValue(a);
      const boxB = boxedValue(b);
      if (boxA) {
        if (!boxB || boxA.index !== boxB.index || !objectIs(boxA.value, boxB.value)) return false;
      } else {
        const hrefA = urlHref ? brand(urlHref, a) : noBrand;
        if (hrefA !== noBrand) {
          if (brand(urlHref, b) !== hrefA) return false;
        } else if (boxB || isError(b) || isWeak(a) || isPromise(a)) {
          return false;
        } else if (types.isKeyObject?.(a) || types.isCryptoKey?.(a) ||
            (cryptoKeyType && brand(cryptoKeyType, a) !== noBrand)) {
          // Node-specific opaque key objects need the host's native equality.
          // Never treat two distinct hidden keys as equal empty objects.
          if (!types.equalCryptoKeys?.(a, b, (x, y) => inner(x, y, skipPrototype, memos))) return false;
        }
      }
    }
    return keyCheck(a, b, skipPrototype, memos, 0);
  }

  function keyCheck(a, b, skipPrototype, memos, iteration, keysB) {
    let keysA;
    if (keysB === undefined) {
      keysA = keys(a);
      keysB = keys(b);
      if (keysA.length !== keysB.length) return false;
      for (const key of symbols(a)) if (enumerable(a, key)) keysA.push(key);
      for (const key of symbols(b)) if (enumerable(b, key)) keysB.push(key);
      if (keysA.length !== keysB.length) return false;
    }
    if (keysB.length === 0 &&
        (iteration === 0 || (iteration === 1 && b.length === 0) || b.size === 0)) return true;
    return handleCycles(a, b, skipPrototype, keysA, keysB, memos, iteration);
  }

  function handleCycles(a, b, skipPrototype, keysA, keysB, memos, iteration) {
    if (memos === undefined) {
      memos = {set: undefined, a, b, c: undefined, d: undefined, deep: false};
      return objectEntries(a, b, skipPrototype, keysA, keysB, memos, iteration);
    }
    if (memos.set === undefined) {
      if (memos.deep === false) {
        if (memos.a === a) return memos.b === b;
        if (memos.b === b) return false;
        memos.c = a;
        memos.d = b;
        memos.deep = true;
        const result = objectEntries(a, b, skipPrototype, keysA, keysB, memos, iteration);
        memos.deep = false;
        if (memos.set !== undefined) {
          memos.set.delete(memos.c);
          memos.set.delete(memos.d);
        }
        return result;
      }
      memos.set = new SafeSet([memos.a, memos.b, memos.c, memos.d]);
    }
    const set = memos.set;
    const size = set.size;
    set.add(a);
    set.add(b);
    if (size !== set.size - 2) return size === set.size;
    const result = objectEntries(a, b, skipPrototype, keysA, keysB, memos, iteration);
    set.delete(a);
    set.delete(b);
    return result;
  }

  function equalSet(a, b, skipPrototype, memos) {
    let unmatched;
    const iteratorB = b.values();
    for (const item of iteratorB) {
      if (a.has(item)) continue;
      if (item === null || typeof item !== 'object') return false;
      if (unmatched === undefined) {
        if (a.size < 3) {
          const iteratorA = a.values();
          const firstA = iteratorA.next().value;
          if (inner(firstA, item, skipPrototype, memos)) {
            if (b.size === 1) return true;
            const secondA = iteratorA.next().value;
            return b.has(secondA) || inner(secondA, iteratorB.next().value, skipPrototype, memos);
          }
          return a.size !== 1 && inner(iteratorA.next().value, item, skipPrototype, memos) &&
              (b.size === 1 || b.has(firstA) || inner(firstA, iteratorB.next().value, skipPrototype, memos));
        }
        unmatched = [];
      }
      unmatched.push(item);
    }
    if (unmatched === undefined) return true;
    let direction = 1;
    let start = 0;
    let end = unmatched.length - 1;
    const extraChecks = unmatched.length !== a.size;
    for (const item of a) {
      if (extraChecks) {
        if (b.has(item)) continue;
        if (typeof item !== 'object') return false;
      }
      let innerStart = start;
      if (direction === 1) {
        if (compareObject(item, unmatched[start], skipPrototype, memos)) {
          ++start;
          continue;
        }
        if (start === end) return false;
        direction = -1;
        ++innerStart;
      }
      if (!compareObject(item, unmatched[end], skipPrototype, memos)) {
        direction = 1;
        let matched = false;
        for (let index = end - 1; index >= innerStart; --index) {
          if (compareObject(item, unmatched[index], skipPrototype, memos)) {
            unmatched[index] = unmatched[end];
            matched = true;
            break;
          }
        }
        if (!matched) return false;
      }
      --end;
    }
    return true;
  }

  function equalMap(a, b, skipPrototype, memos) {
    let unmatched;
    for (const [key, value] of b) {
      if (key !== null && typeof key === 'object') {
        if (unmatched === undefined) {
          if (a.size === 1) {
            const [firstKey, firstValue] = a.entries().next().value;
            return inner(firstKey, key, skipPrototype, memos) && inner(firstValue, value, skipPrototype, memos);
          }
          unmatched = [];
        }
        unmatched.push(key);
      } else {
        const firstValue = a.get(key);
        if ((firstValue === undefined && !a.has(key)) || !inner(firstValue, value, skipPrototype, memos)) return false;
      }
    }
    if (unmatched === undefined) return true;
    let direction = 1;
    let start = 0;
    let end = unmatched.length - 1;
    const extraChecks = unmatched.length !== a.size;
    for (const [key, value] of a) {
      if (extraChecks && (key === null || typeof key !== 'object')) {
        if (b.has(key)) continue;
        return false;
      }
      let innerStart = start;
      if (direction === 1) {
        const otherKey = unmatched[start];
        if (compareObject(key, otherKey, skipPrototype, memos) && inner(value, b.get(otherKey), skipPrototype, memos)) {
          ++start;
          continue;
        }
        if (start === end) return false;
        direction = -1;
        ++innerStart;
      }
      const otherKey = unmatched[end];
      if (!compareObject(key, otherKey, skipPrototype, memos) || !inner(value, b.get(otherKey), skipPrototype, memos)) {
        direction = 1;
        let matched = false;
        for (let index = end - 1; index >= innerStart; --index) {
          const otherKey = unmatched[index];
          if (compareObject(key, otherKey, skipPrototype, memos) && inner(value, b.get(otherKey), skipPrototype, memos)) {
            unmatched[index] = unmatched[end];
            matched = true;
            break;
          }
        }
        if (!matched) return false;
      }
      --end;
    }
    return true;
  }

  function objectEntries(a, b, skipPrototype, keysA, keysB, memos, iteration) {
    let index = 0;
    if (keysA !== undefined) {
      for (; index < keysB.length; ++index) {
        const key = keysB[index];
        if (keysA[index] !== key) break;
        if (!inner(a[key], b[key], skipPrototype, memos)) return false;
      }
    }
    for (; index < keysB.length; ++index) {
      const key = keysB[index];
      const property = descriptor(a, key);
      if (!property || property.enumerable !== true) return false;
      if (!inner(property.writable !== undefined ? property.value : a[key], b[key], skipPrototype, memos)) return false;
    }
    if (iteration === 1) {
      for (let i = 0; i < a.length; ++i) {
        if (b[i] === undefined) {
          if (!own(b, i)) {
            const sparseA = keys(a);
            const sparseB = keys(b);
            if (sparseA.length !== sparseB.length) return false;
            for (; i < sparseB.length; ++i) {
              const key = sparseB[i];
              if ((a[key] === undefined && !own(a, key)) || !inner(a[key], b[key], skipPrototype, memos)) return false;
            }
            return true;
          }
          if (a[i] !== undefined || !own(a, i)) return false;
        } else if (a[i] === undefined || !inner(a[i], b[i], skipPrototype, memos)) return false;
      }
    } else if (iteration === 2) return equalSet(a, b, skipPrototype, memos);
    else if (iteration === 3) return equalMap(a, b, skipPrototype, memos);
    return true;
  }

  return function isDeepStrictEqual(a, b, skipPrototype) {
    return inner(a, b, Boolean(skipPrototype), undefined);
  };
}

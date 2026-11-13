// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const util = require('node:util');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, 'common/deep_equal.js'), 'utf8');
const create = vm.runInThisContext(`(() => { ${source}; return createIsDeepStrictEqual; })()`);
const equal = create({isPromise: util.types.isPromise, isNativeError: util.types.isNativeError});
const fallback = create();
const outcome = operation => {
  try { return {value: operation()}; }
  catch (error) { return {error: error.name, code: error.code}; }
};
function differential(a, b, message, compare = equal) {
  for (const options of [undefined, false, true]) {
    assert.deepEqual(outcome(() => compare(a, b, options)),
      outcome(() => util.isDeepStrictEqual(a, b, options)), `${message}; skipPrototype=${options}`);
    assert.deepEqual(outcome(() => compare(b, a, options)),
      outcome(() => util.isDeepStrictEqual(b, a, options)), `${message} reversed; skipPrototype=${options}`);
  }
}

test('strict comparison matches Node for primitive values and function identity', () => {
  const shared = Symbol('shared');
  const values = [undefined, null, true, false, 0, -0, NaN, Infinity, -Infinity, 1, 1n,
    '', '1', 'NaN', shared, Symbol.for('test'), Symbol('shared'), () => 1, () => 1];
  for (let i = 0; i < values.length; ++i) {
    for (let j = 0; j < values.length; ++j) differential(values[i], values[j], `primitive ${i}/${j}`);
  }
});

test('strict comparison matches Node for prototypes, constructors and skipPrototype', () => {
  class First { constructor() { this.value = 1; } }
  class Second { constructor() { this.value = 1; } }
  const cases = [
    [{value: 1}, Object.assign(Object.create(null), {value: 1})],
    [new First(), new Second()], [new First(), {value: 1}],
    [Object.create({constructor: Object}), {}],
    [Object.create({constructor: undefined}), Object.create({constructor: undefined})],
    [{constructor: undefined}, {constructor: undefined}],
    [Object.assign(Object.create(null), {constructor: 17}), {constructor: 17}],
    [Object.create({constructor: First}), Object.create({constructor: Second})],
  ];
  for (const [a, b] of cases) differential(a, b, 'prototype');
  assert.equal(equal(new First(), new Second(), {}), util.isDeepStrictEqual(new First(), new Second(), {}));
});

test('owned enumerable strings and symbols match Node, including undefined and accessors', () => {
  const symbol = Symbol('property');
  const otherSymbol = Symbol('property');
  const cases = [
    [{a: 1, b: 2}, {b: 2, a: 1}], [{a: undefined}, {b: undefined}],
    [{[symbol]: 1}, {[symbol]: 1}], [{[symbol]: 1}, {[otherSymbol]: 1}],
    [Object.defineProperty({}, symbol, {value: 1}), {}],
    [Object.defineProperty({}, 'hidden', {value: 1}), Object.defineProperty({}, 'hidden', {value: 2})],
    [Object.defineProperty({}, 'x', {value: 1, enumerable: true}), {get x() { return 1; }}],
    [{x: undefined}, Object.create({x: undefined})],
    [{a: {b: 1}}, {a: {b: 2}}],
  ];
  for (const [a, b] of cases) differential(a, b, 'properties');
  const throwing = {get x() { throw new RangeError('getter'); }};
  differential(throwing, {x: 1}, 'throwing getter');
});

test('dense and sparse arrays preserve holes, extra keys and enumerable symbols', () => {
  const symbol = Symbol('array');
  const arrays = [[], [undefined], Array(1), [null], [1], [1, 2], [2, 1], [1, , 3],
    [1, undefined, 3], Object.assign([], {x: 1}), Object.assign([1], {[symbol]: 2}),
    Object.defineProperty([1], '0', {value: 2, enumerable: false}),
    Object.assign([], {'4294967295': 1}), Object.assign([], {'01': 1})];
  for (let i = 0; i < arrays.length; ++i) {
    for (let j = 0; j < arrays.length; ++j) differential(arrays[i], arrays[j], `array ${i}/${j}`);
  }
});

test('dates, regexp source flags and lastIndex match Node', () => {
  const second = /value/g;
  second.lastIndex = 2;
  const values = [new Date(0), new Date(0), new Date(1), new Date(NaN), new Date(NaN),
    /value/g, /value/g, /value/i, /other/g, second,
    Object.assign(new Date(0), {extra: 1}), Object.assign(/value/g, {extra: 1})];
  for (let i = 0; i < values.length; ++i) {
    for (let j = 0; j < values.length; ++j) differential(values[i], values[j], `date/regexp ${i}/${j}`);
  }
});

test('ArrayBuffer, DataView and typed arrays compare actual bytes and extra properties', () => {
  const buffer = new Uint8Array([9, 1, 2, 9]).buffer;
  const symbol = Symbol('view');
  const values = [new Uint8Array([1, 2]), new Uint8Array([1, 2]), new Uint8Array([2, 1]),
    new Uint8Array(buffer, 1, 2), new Int8Array([1, 2]), new Uint16Array([513]),
    new DataView(buffer, 1, 2), new DataView(new Uint8Array([1, 2]).buffer),
    new Uint8Array([1, 2]).buffer, new Uint8Array([1, 2]).buffer,
    new Float32Array([0]), new Float32Array([-0]), new Float32Array([NaN]),
    new Float32Array([NaN]), new Float64Array([NaN]),
    new BigInt64Array([1n]), new BigUint64Array([1n]),
    Object.assign(new Uint8Array([1, 2]), {[symbol]: 1}),
    Object.assign(new Uint8Array([1, 2]), {extra: 1}),
    Buffer.from([1, 2]), Buffer.from([1, 2])];
  if (typeof Float16Array === 'function') values.push(new Float16Array([NaN]), new Float16Array([NaN]));
  if (typeof SharedArrayBuffer === 'function') {
    const a = new SharedArrayBuffer(2), b = new SharedArrayBuffer(2);
    new Uint8Array(a).set([1, 2]); new Uint8Array(b).set([1, 2]);
    values.push(a, b);
  }
  for (let i = 0; i < values.length; ++i) {
    for (let j = 0; j < values.length; ++j) differential(values[i], values[j], `bytes ${i}/${j}`);
  }
  const nanA = new Float64Array(1), nanB = new Float64Array(1);
  new DataView(nanA.buffer).setBigUint64(0, 0x7ff8000000000001n, true);
  new DataView(nanB.buffer).setBigUint64(0, 0x7ff8000000000002n, true);
  differential(nanA, nanB, 'different NaN payloads');
});

test('boxed primitives retain type and Object.is numeric semantics', () => {
  const shared = Symbol('boxed');
  const values = [Object(1), Object(1), Object(0), Object(-0), Object(NaN), Object(NaN),
    Object('1'), Object('1'), Object(true), Object(false), Object(1n), Object(2n),
    Object(shared), Object(shared), Object(Symbol('boxed')), {},
    Object.assign(Object(1), {extra: 1})];
  for (let i = 0; i < values.length; ++i) {
    for (let j = 0; j < values.length; ++j) differential(values[i], values[j], `boxed ${i}/${j}`);
  }
});

test('detached and resizable buffers preserve Node byte comparison behavior', () => {
  for (const kind of ['buffer', 'typed', 'view']) {
    const a = new ArrayBuffer(4), b = new ArrayBuffer(4);
    const valueA = kind === 'buffer' ? a : kind === 'typed' ? new Uint8Array(a) : new DataView(a);
    const valueB = kind === 'buffer' ? b : kind === 'typed' ? new Uint8Array(b) : new DataView(b);
    structuredClone(a, {transfer: [a]}); structuredClone(b, {transfer: [b]});
    differential(valueA, valueB, `detached ${kind}`);
  }
  if (typeof ArrayBuffer.prototype.resize === 'function') {
    const a = new ArrayBuffer(4, {maxByteLength: 16});
    const b = new ArrayBuffer(4, {maxByteLength: 32});
    differential(a, b, 'resizable buffers ignore capacity');
    const viewA = new Uint8Array(a, 2, 2), viewB = new Uint8Array(b, 2, 2);
    a.resize(1); b.resize(1);
    differential(viewA, viewB, 'out of bounds typed arrays');
  }
});

test('Errors compare semantic fields and AggregateError members but ignore hidden stack', () => {
  const first = new Error('same'), second = new Error('same');
  first.stack = 'stack one'; second.stack = 'stack two';
  const values = [first, second, new Error('different'), new TypeError('same'),
    new Error('same', {cause: undefined}), new Error('same', {cause: {x: 1}}),
    new Error('same', {cause: {x: 1}}), new Error('same', {cause: {x: 2}}),
    new AggregateError([new Error('one'), 2], 'same'),
    new AggregateError([new Error('one'), 2], 'same'),
    new AggregateError([2, new Error('one')], 'same'),
    Object.assign(new Error('same'), {extra: 1}),
    Object.defineProperty(new Error('same'), 'stack', {value: 'visible stack', enumerable: true})];
  for (let i = 0; i < values.length; ++i) {
    for (let j = 0; j < values.length; ++j) differential(values[i], values[j], `error ${i}/${j}`);
  }
  const a = new Error('cycle'), b = new Error('cycle');
  a.cause = a; b.cause = b;
  differential(a, b, 'enumerable error cause cycle');
});

test('Maps and Sets match Node for unordered object contents and duplicate structural values', () => {
  const shared = {id: 1};
  const values = [new Set(), new Map(), new Set([1, 2]), new Set([2, 1]),
    new Set([{id: 1}, {id: 2}]), new Set([{id: 2}, {id: 1}]),
    new Set([{id: 1}, {id: 1}]), new Set([shared, {id: 2}]),
    new Set([shared, {id: 1}]), new Set([null, undefined]),
    new Set([NaN, -0]), new Set([NaN, +0]),
    new Map([[1, 'one'], [2, 'two']]), new Map([[2, 'two'], [1, 'one']]),
    new Map([[{id: 1}, 'a'], [{id: 1}, 'b']]),
    new Map([[{id: 1}, 'b'], [{id: 1}, 'a']]),
    new Map([[{id: 1}, 'a'], [{id: 1}, 'a']]),
    new Map([[undefined, undefined]]), new Map([[null, undefined]]),
    Object.assign(new Set([1]), {extra: 1}), Object.assign(new Map([[1, 2]]), {extra: 1})];
  for (let i = 0; i < values.length; ++i) {
    for (let j = 0; j < values.length; ++j) differential(values[i], values[j], `collection ${i}/${j}`);
  }
});

test('cycles and repeated references use Node structural comparison semantics', () => {
  function cycle(length) { const root = {}; let next = root;
    for (let i = 1; i < length; ++i) next = next.next = {};
    next.next = root; return root; }
  const a = {}, b = {}; a.self = a; b.self = b;
  const repeated = {};
  const values = [a, b, cycle(1), cycle(2), cycle(3), cycle(4),
    {a: repeated, b: repeated}, {a: {}, b: {}},
    {inner: a}, {inner: b}];
  const setA = new Set(), setB = new Set(); setA.add(setA); setB.add(setB);
  const mapA = new Map(), mapB = new Map(); mapA.set(mapA, mapA); mapB.set(mapB, mapB);
  const arrayA = [], arrayB = []; arrayA.push(arrayA); arrayB.push(arrayB);
  values.push(setA, setB, mapA, mapB, arrayA, arrayB);
  for (let i = 0; i < values.length; ++i) {
    for (let j = 0; j < values.length; ++j) differential(values[i], values[j], `cycle ${i}/${j}`);
  }
});

test('deterministic cyclic collection graphs agree with Node after insertion-order changes', () => {
  let state = 0x13579bdf;
  const random = limit => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state % limit;
  };
  for (let sample = 0; sample < 240; ++sample) {
    const nodes = Array.from({length: 5}, (_, i) => i < 2 ? new Map() : i < 4 ? new Set() : {});
    const values = [...nodes, null, undefined, 1, 2, 'same'];
    for (const node of nodes) {
      for (let i = 0; i < 3; ++i) {
        if (node instanceof Map) node.set(values[random(values.length)], values[random(values.length)]);
        else if (node instanceof Set) node.add(values[random(values.length)]);
        else node['key' + i] = values[random(values.length)];
      }
    }
    const cloned = structuredClone(nodes);
    for (const node of cloned) {
      if (node instanceof Map) { const entries = [...node].reverse(); node.clear(); for (const entry of entries) node.set(...entry); }
      if (node instanceof Set) { const entries = [...node].reverse(); node.clear(); for (const entry of entries) node.add(entry); }
    }
    differential(nodes, cloned, `graph ${sample}`);
    cloned[4].extra = sample;
    differential(nodes, cloned, `changed graph ${sample}`);
  }
});

test('weak collections and Promises use identity without evaluating thenables', () => {
  const then = () => { throw new Error('must not execute a thenable'); };
  const values = [new WeakMap(), new WeakMap(), new WeakSet(), new WeakSet(),
    Promise.resolve(1), Promise.resolve(1), {then}, {then}];
  for (let i = 0; i < values.length; ++i) {
    for (let j = 0; j < values.length; ++j) {
      differential(values[i], values[j], `opaque ${i}/${j}`);
      differential(values[i], values[j], `fallback opaque ${i}/${j}`, fallback);
    }
  }
  differential(Object.create(Promise.prototype), Object.create(Promise.prototype), 'native Promise brand hook');
});

test('built-in brand checks do not mistake Symbol.toStringTag objects for builtins', () => {
  for (const tag of ['Date', 'RegExp', 'Set', 'Map', 'WeakMap', 'WeakSet', 'Promise',
    'ArrayBuffer', 'SharedArrayBuffer', 'Uint8Array', 'DataView', 'Number', 'Error']) {
    differential({[Symbol.toStringTag]: tag}, {[Symbol.toStringTag]: tag}, tag);
    differential({[Symbol.toStringTag]: tag}, {[Symbol.toStringTag]: tag}, tag, fallback);
  }
});

test('cross realm builtins compare with Node prototype and skipPrototype rules', () => {
  const expressions = ['({x:1})', '[1,2]', 'new Date(1)', '/value/g', 'new Map([[{x:1},2]])',
    'new Set([{x:1}])', 'new Uint8Array([1,2])', 'new DataView(new Uint8Array([1,2]).buffer)',
    'Object(1n)', 'new Error("same")', 'Promise.resolve(1)', 'new WeakMap()'];
  for (const expression of expressions) {
    const a = vm.runInNewContext(expression);
    const b = vm.runInNewContext(expression);
    const local = vm.runInThisContext(expression);
    differential(a, b, expression);
    differential(a, local, expression + ' local');
    differential(a, b, expression + ' fallback', fallback);
  }
});

test('URL values and enumerable additions compare with Node', () => {
  differential(new URL('https://example.test/a'), new URL('https://example.test/a'), 'URL same');
  differential(new URL('https://example.test/a'), new URL('https://example.test/b'), 'URL different');
  differential(Object.assign(new URL('https://example.test/a'), {extra: 1}),
    new URL('https://example.test/a'), 'URL own property');
});

test('comparison factory runs without Node modules or Buffer in a renderer realm', () => {
  const context = vm.createContext({});
  vm.runInContext(source, context);
  const result = vm.runInContext(`(() => {
    const equal = createIsDeepStrictEqual();
    const a = {map: new Map([[{key: 1}, new Uint8Array([2, 3])]])};
    const b = {map: new Map([[{key: 1}, new Uint8Array([2, 3])]])};
    a.self = a; b.self = b;
    return [equal(a, b), equal(new Set([1]), new Set([2])), equal(0, -0)];
  })()`, context);
  assert.deepEqual(Array.from(result), [true, false, false]);
});

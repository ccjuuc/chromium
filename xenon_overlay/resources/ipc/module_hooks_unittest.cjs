// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {spawnSync} = require('node:child_process');
const test = require('node:test');

function create() {
  let changes = 0;
  const context = vm.createContext({changed: () => ++changes,
    decodeSource: source => new TextDecoder().decode(source)});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'main/module_hooks.js'), 'utf8') +
    '\nglobalThis.hooks = createSynchronousModuleHooks({changed, decodeSource});', context);
  return {hooks: context.hooks, changes: () => changes};
}
const initial = () => ({parentURL: 'file:///application/main.cjs',
  conditions: ['node', 'require'], importAttributes: {}});
const baseResolve = specifier => ({url: 'file:///application/' + specifier, format: 'commonjs'});

test('registration freezes hook metadata and deregistration is idempotent', () => {
  const {hooks, changes} = create();
  const resolve = (specifier, context, next) => next(specifier, context);
  const registration = hooks.registerHooks({resolve});
  assert.equal(Object.isFrozen(registration), true);
  assert.equal(registration.resolve, resolve);
  assert.equal(Object.hasOwn(registration, 'load'), true);
  assert.equal(hooks.hasResolveHooks(), true);
  assert.equal(changes(), 1);
  registration.deregister();
  registration.deregister();
  assert.equal(hooks.hasResolveHooks(), false);
  assert.equal(changes(), 2);
});

test('resolve hooks execute last registered first and merge partial next context', () => {
  const {hooks} = create();
  const trace = [];
  hooks.registerHooks({resolve(specifier, context, next) {
    trace.push(['first', specifier, context.parentURL, ...context.conditions]);
    return next(specifier + '.cjs');
  }});
  hooks.registerHooks({resolve(specifier, context, next) {
    trace.push(['second', specifier]);
    const result = next(specifier, {parentURL: 'file:///another/main.cjs'});
    return {...result, customField: true};
  }});
  const result = hooks.resolve('target', initial(), (specifier, context) => {
    trace.push(['default', specifier, context.parentURL]);
    return baseResolve(specifier);
  });
  assert.deepEqual(trace, [['second', 'target'],
    ['first', 'target', 'file:///another/main.cjs', 'node', 'require'],
    ['default', 'target.cjs', 'file:///another/main.cjs']]);
  assert.equal(result.url, 'file:///application/target.cjs');
  assert.equal(Object.getPrototypeOf(result), null);
  assert.equal(Object.hasOwn(result, 'customField'), false);
});

test('shortCircuit is required when a hook omits next', () => {
  for (const kind of ['resolve', 'load']) {
    const {hooks} = create();
    hooks.registerHooks({[kind]() { return {url: 'custom:item', format: 'module', source: 'export {}'}; }});
    assert.throws(() => hooks[kind]('custom:item', initial(), () => {
      throw new Error('default must not execute');
    }), {code: 'ERR_INVALID_RETURN_PROPERTY_VALUE'});
  }
  const {hooks} = create();
  hooks.registerHooks({resolve: () => ({url: 'custom:item', shortCircuit: true})});
  assert.equal(hooks.resolve('item', initial(), baseResolve).url, 'custom:item');
});

test('load chains preserve source transformations and decode the exact byte view', () => {
  const {hooks} = create();
  hooks.registerHooks({load(url, context, next) {
    const original = next(url, context);
    const bytes = Buffer.from('xx' + original.source + ';module.exports.extra=2;yy');
    return {...original, source: new DataView(bytes.buffer, bytes.byteOffset + 2, bytes.length - 4)};
  }});
  const result = hooks.load('file:///app/item.cjs', {format: 'commonjs'},
    () => ({format: 'commonjs', source: 'module.exports={value:1}'}));
  assert.equal(result.source, 'module.exports={value:1};module.exports.extra=2;');
});

test('builtins allow null source while ordinary module source is validated', () => {
  const {hooks} = create();
  hooks.registerHooks({load: (url, context, next) => next(url, context)});
  assert.equal(hooks.load('node:fs', {format: 'builtin'},
    () => ({format: 'builtin', source: null})).source, null);
  assert.throws(() => hooks.load('file:///item.cjs', {format: 'commonjs'},
    () => ({format: 'commonjs', source: null})), {code: 'ERR_INVALID_RETURN_PROPERTY_VALUE'});
});

test('promise returns are rejected and original thrown values are preserved', () => {
  const {hooks} = create();
  const registration = hooks.registerHooks({resolve: async () => ({url: 'custom:item', shortCircuit: true})});
  assert.throws(() => hooks.resolve('item', initial(), baseResolve),
    {code: 'ERR_INVALID_RETURN_PROPERTY_VALUE'});
  registration.deregister();
  const error = {application: 'error'};
  hooks.registerHooks({resolve: () => { throw error; }});
  assert.throws(() => hooks.resolve('item', initial(), baseResolve), thrown => thrown === error);
});

test('deregistration during a load preserves the current captured chain', () => {
  const {hooks} = create();
  const trace = [];
  const first = hooks.registerHooks({resolve(specifier, context, next) {
    trace.push('first'); return next(specifier, context);
  }});
  hooks.registerHooks({resolve(specifier, context, next) {
    trace.push('second'); first.deregister(); return next(specifier, context);
  }});
  hooks.resolve('one', initial(), baseResolve);
  hooks.resolve('two', initial(), baseResolve);
  assert.deepEqual(trace, ['second', 'first', 'second']);
});

test('empty registration and invalid hook properties follow Node contract', () => {
  const {hooks, changes} = create();
  assert.equal(hooks.registerHooks({resolve: false}).resolve, false);
  assert.equal(changes(), 0);
  for (const value of [1, {}, 'hook']) {
    assert.throws(() => hooks.registerHooks({resolve: value}), {code: 'ERR_INVALID_ARG_TYPE'});
  }
});

test('resolve/load ordering and context updates match native Node registerHooks', () => {
  const source = String.raw`
    const {registerHooks} = require('node:module');
    const trace = [];
    const first = registerHooks({
      resolve(specifier, context, next) {
        if (specifier === 'fixture') {
          trace.push(['first-resolve', context.parentURL, context.conditions.includes('require')]);
          return {url: 'fixture:module', format: 'commonjs', shortCircuit: true};
        }
        return next(specifier, context);
      },
      load(url, context, next) {
        if (url === 'fixture:module') {
          trace.push(['first-load', context.format]);
          return {format: 'commonjs', source: 'module.exports = 42', shortCircuit: true};
        }
        return next(url, context);
      }
    });
    const second = registerHooks({resolve(specifier, context, next) {
      if (specifier === 'fixture') { trace.push(['second-resolve']); return next(specifier, {parentURL: 'file:///changed.cjs'}); }
      return next(specifier, context);
    }, load(url, context, next) {
      if (url === 'fixture:module') trace.push(['second-load']);
      return next(url, context);
    }});
    const value = require('fixture');
    first.deregister(); second.deregister();
    console.log(JSON.stringify({trace, value}));
  `;
  const oracle = spawnSync(process.execPath, ['-e', source], {encoding: 'utf8'});
  assert.equal(oracle.status, 0, oracle.stderr);
  const {hooks} = create();
  const trace = [];
  hooks.registerHooks({resolve(specifier, context) {
    trace.push(['first-resolve', context.parentURL, context.conditions.includes('require')]);
    return {url: 'fixture:module', format: 'commonjs', shortCircuit: true};
  }, load(url, context) {
    trace.push(['first-load', context.format]);
    return {format: 'commonjs', source: 'module.exports = 42', shortCircuit: true};
  }});
  hooks.registerHooks({resolve(specifier, context, next) {
    trace.push(['second-resolve']); return next(specifier, {parentURL: 'file:///changed.cjs'});
  }, load(url, context, next) {
    trace.push(['second-load']); return next(url, context);
  }});
  const resolved = hooks.resolve('fixture', initial(), baseResolve);
  const loaded = hooks.load(resolved.url, {format: resolved.format}, () => { throw Error('unexpected default'); });
  const module = {exports: {}};
  new Function('module', loaded.source)(module);
  assert.deepEqual({trace, value: module.exports}, JSON.parse(oracle.stdout));
});

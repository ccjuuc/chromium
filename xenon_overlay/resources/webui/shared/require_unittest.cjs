// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const ts = require('../../../../third_party/node/node_modules/typescript');

const source = ts.transpileModule(
    fs.readFileSync(path.join(__dirname, 'require.ts'), 'utf8'),
    {compilerOptions: {target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS}}).outputText
    // The module itself exports a Node-style require(). Load its single static
    // Mojo dependency separately instead of letting that name shadow CJS.
    .replace('require("./xenon_node.mojom-webui.js")', '__loadBindings()');

function create(openNativeFileDialog) {
  class PageCallbackRouter {
    constructor() {
      return new Proxy({$: {bindNewPipeAndPassRemote() {}}}, {
        get: (target, key) => target[key] ?? {addListener() {}},
      });
    }
  }
  class PageHandlerRemote {
    constructor() {
      this.$ = {bindNewPipeAndPassReceiver() {}};
      this.openNativeFileDialog = openNativeFileDialog;
    }
  }
  const exports = {};
  const context = vm.createContext({exports, window: {}, navigator: {platform: 'Win32'},
    __loadBindings: () => ({PageCallbackRouter, PageHandlerRemote,
      PageHandlerFactory: {getRemote: () => ({createPageHandler() {}})}}),
  });
  vm.runInContext(source, context, {filename: 'shared/require.ts'});
  return {open: exports.openNativeFileDialog, options: json => {
    context.input = JSON.stringify(json);
    return vm.runInContext('JSON.parse(input)', context);
  }};
}

function response(canceled, paths) {
  return {error: '', result: {dictionaryValue: {storage: {
    canceled: {boolValue: canceled},
    filePaths: {listValue: {storage: paths.map(path => ({stringValue: path}))}},
  }}}};
}

test('WebUI open dialog uses the current Mojo shape and preserves complete options', async () => {
  let request;
  let complete;
  const runtime = create(options => {
    request = options;
    return new Promise(resolve => { complete = resolve; });
  });
  const options = runtime.options({title: 'Open', defaultPath: '/media',
    filters: [{name: 'Video', extensions: ['mp4', 'mkv']}],
    properties: ['openFile', 'multiSelections']});
  const pending = runtime.open(options);
  options.defaultPath = 'changed';
  options.filters[0].extensions.push('changed');
  const fields = request.dictionaryValue.storage;
  assert.equal(fields.defaultPath.stringValue, '/media');
  assert.deepEqual(Array.from(fields.properties.listValue.storage, v => v.stringValue),
      ['openFile', 'multiSelections']);
  const filter = fields.filters.listValue.storage[0].dictionaryValue.storage;
  assert.equal(filter.name.stringValue, 'Video');
  assert.deepEqual(Array.from(filter.extensions.listValue.storage, v => v.stringValue), ['mp4', 'mkv']);
  complete(response(false, ['/media/one.mp4', '/media/two.mkv']));
  assert.deepEqual(Array.from(await pending), ['/media/one.mp4', '/media/two.mkv']);
});

test('WebUI open dialog distinguishes cancellation, owner abort and malformed responses', async () => {
  let reply = response(true, []);
  const runtime = create(async () => reply);
  assert.deepEqual(Array.from(await runtime.open()), []);
  reply = {result: {nullValue: 0}, error: 'ERR_ABORTED: owner changed'};
  await assert.rejects(runtime.open(), {code: 'ERR_ABORTED'});
  reply = {result: {nullValue: 0}, error: ''};
  await assert.rejects(runtime.open(), {code: 'ERR_FAILED'});
  await assert.rejects(runtime.open('invalid'), {code: 'ERR_INVALID_ARG_TYPE'});
});

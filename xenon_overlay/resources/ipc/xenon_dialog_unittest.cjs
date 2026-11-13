// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const context = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, 'main/dialog.js'), 'utf8'), context);
const plain = value => JSON.parse(JSON.stringify(value));
class BrowserWindow {
  constructor(id = 7) { this.id = id; this.destroyed = false; }
  isDestroyed() { return this.destroyed; }
}
function create(options = {}) {
  const calls = [];
  const call = (id, command, details) => {
    calls.push({id, command, details: plain(details)});
    return command === 'electron-message-box' ? {response: 2, checkboxChecked: true} :
      {canceled: false, filePath: 'C:\\drawings\\example.drawio'};
  };
  return {calls, dialog: context.createElectronDialogs({BrowserWindow, callSync: call,
    callAsync: call, isReady: () => true, platform: 'win32', ...options})};
}

test('message boxes preserve owner, arbitrary buttons, selected index and checkbox result', async () => {
  const {dialog, calls} = create();
  const options = {message: 'Save changes?', detail: 'Unsaved drawing',
    buttons: ['&Save', '&Discard Changes', 'Cancel'], defaultId: 0, cancelId: 2,
    normalizeAccessKeys: true, noLink: true, checkboxLabel: 'Remember', checkboxChecked: true};
  assert.equal(dialog.showMessageBoxSync(new BrowserWindow(), options), 2);
  assert.deepEqual(await dialog.showMessageBox(null, options), {response: 2, checkboxChecked: true});
  assert.equal(calls[0].id, 7);
  assert.equal(calls[1].id, 0);
  assert.equal(calls[0].command, 'electron-message-box');
  assert.deepEqual(calls[0].details, {...options, type: 'none', title: ''});
});

test('cancel defaults match Electron and explicit -1 is never replaced by a destructive button', () => {
  for (const [options, expected] of [
    [{}, 0], [{buttons: ['Save', 'Discard'], defaultId: 0}, 1],
    [{buttons: ['Save', 'Cancel', 'No']}, 1], [{buttons: ['Save', 'No'], cancelId: 0}, 0],
    [{buttons: ['Delete', 'Cancel'], cancelId: -1}, -1],
    [{buttons: ['Save', 'Cancel'], cancelId: null}, 1],
  ]) {
    const {dialog, calls} = create();
    dialog.showMessageBoxSync(options);
    assert.equal(calls[0].details.cancelId, expected);
    assert.equal(calls[0].details.defaultId, options.defaultId ?? -1);
  }
  const mac = create({platform: 'darwin'});
  mac.dialog.showMessageBoxSync({buttons: ['Save', '&Cancel'], normalizeAccessKeys: true});
  assert.equal(mac.calls[0].details.cancelId, 1);
  const win = create();
  win.dialog.showMessageBoxSync({buttons: ['Save', '&Cancel'], normalizeAccessKeys: true});
  assert.equal(win.calls[0].details.cancelId, 0);
});

test('asynchronous save waits for user choice and snapshots nested options before yielding', async () => {
  let finish;
  const calls = [];
  const {dialog} = create({callAsync: (id, command, details) => {
    calls.push({id, command, details: plain(details)});
    return new Promise(resolve => { finish = resolve; });
  }});
  const options = {defaultPath: 'drawing.drawio', filters: [{name: 'Drawing', extensions: ['drawio', 'xml']}],
    properties: ['showOverwriteConfirmation']};
  let settled = false;
  const result = dialog.showSaveDialog(new BrowserWindow(9), options).then(value => { settled = true; return value; });
  options.defaultPath = 'changed';
  options.filters[0].extensions[0] = 'changed';
  options.properties.push('changed');
  await Promise.resolve();
  assert.equal(settled, false);
  assert.deepEqual(calls, [{id: 9, command: 'electron-save-dialog', details: {
    defaultPath: 'drawing.drawio', filters: [{name: 'Drawing', extensions: ['drawio', 'xml']}],
    properties: ['showOverwriteConfirmation'],
  }}]);
  finish({canceled: false, filePath: 'C:\\chosen\\drawing.drawio'});
  assert.deepEqual(await result, {canceled: false, filePath: 'C:\\chosen\\drawing.drawio'});
});

test('save cancellation returns an empty async path and undefined synchronously', async () => {
  const cancel = () => ({canceled: true, filePath: ''});
  const {dialog} = create({callSync: cancel, callAsync: cancel});
  assert.equal(dialog.showSaveDialogSync({}), undefined);
  assert.deepEqual(await dialog.showSaveDialog({}), {canceled: true, filePath: ''});
  const selected = create();
  assert.equal(selected.dialog.showSaveDialogSync(undefined, {defaultPath: 'draft'}),
      'C:\\drawings\\example.drawio');
});

test('native abort and access errors retain their codes and are not treated as cancellation', async () => {
  const {dialog} = create({
    callSync: () => { throw new Error('ERR_ACCESS_DENIED: wrong owner'); },
    callAsync: () => Promise.reject(new Error('ERR_ABORTED: owner closed')),
  });
  assert.throws(() => dialog.showMessageBoxSync({}), {code: 'ERR_ACCESS_DENIED'});
  assert.throws(() => dialog.showSaveDialogSync({}), {code: 'ERR_ACCESS_DENIED'});
  assert.throws(() => dialog.showOpenDialogSync({}), {code: 'ERR_ACCESS_DENIED'});
  await assert.rejects(dialog.showMessageBox({}), {code: 'ERR_ABORTED'});
  await assert.rejects(dialog.showSaveDialog({}), {code: 'ERR_ABORTED'});
  await assert.rejects(dialog.showOpenDialog({}), {code: 'ERR_ABORTED'});
});

test('invalid options and stale or forged owners fail synchronously before reaching native UI', () => {
  const {dialog, calls} = create();
  for (const method of ['showMessageBox', 'showMessageBoxSync', 'showSaveDialog',
    'showSaveDialogSync', 'showOpenDialog', 'showOpenDialogSync']) {
    assert.throws(() => dialog[method]('options'), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => dialog[method]({id: 7}, {}), {code: 'ERR_INVALID_ARG_TYPE'});
    const destroyed = new BrowserWindow();
    destroyed.destroyed = true;
    assert.throws(() => dialog[method](destroyed, {}), {code: 'ERR_OBJECT_DESTROYED'});
  }
  for (const options of [{buttons: 'OK'}, {buttons: [5]}, {title: false}, {checkboxChecked: true}])
    assert.throws(() => dialog.showMessageBox(options), {code: 'ERR_INVALID_ARG_TYPE'});
  for (const options of [{defaultId: 5}, {cancelId: -2}, {defaultId: 1.5}])
    assert.throws(() => dialog.showMessageBox(options), {code: 'ERR_OUT_OF_RANGE'});
  for (const options of [{icon: {}}, {signal: {}}, {textWidth: 100}])
    assert.throws(() => dialog.showMessageBox(options), {code: 'ERR_NOT_SUPPORTED'});
  for (const options of [{defaultPath: 'bad\0path'}, {filters: [{name: 'File', extensions: ['.xml']}]},
    {filters: [{name: 'File', extensions: []}]}, {properties: 'createDirectory'}])
    assert.throws(() => dialog.showSaveDialog(options), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls.length, 0);
});

test('readiness and optional empty fields are handled before native UI', () => {
  const beforeReady = create({isReady: () => false});
  assert.throws(() => beforeReady.dialog.showMessageBox({}), {code: 'ERR_NOT_READY'});
  assert.throws(() => beforeReady.dialog.showSaveDialog({}), {code: 'ERR_NOT_READY'});
  assert.throws(() => beforeReady.dialog.showOpenDialog({}), {code: 'ERR_NOT_READY'});
  const {dialog, calls} = create();
  dialog.showMessageBoxSync({icon: null, signal: null, textWidth: 0});
  assert.equal('icon' in calls[0].details, false);
  assert.equal('signal' in calls[0].details, false);
  assert.equal('textWidth' in calls[0].details, false);
});

test('open snapshots options and waits for asynchronous selection without a synchronous host call', async () => {
  let finish;
  const calls = [];
  const {dialog} = create({
    callSync: () => { throw new Error('async open must not block the application'); },
    callAsync: (id, command, details) => {
      calls.push({id, command, details: plain(details)});
      return new Promise(resolve => { finish = resolve; });
    },
  });
  const options = {title: 'Open media', defaultPath: '/media',
    properties: ['openFile', 'multiSelections'],
    filters: [{name: 'Videos', extensions: ['mp4', 'mkv']}]};
  const expected = plain(options);
  let settled = false;
  const pending = dialog.showOpenDialog(new BrowserWindow(8), options)
    .then(value => { settled = true; return value; });
  options.defaultPath = 'changed';
  options.filters[0].extensions.push('changed');
  options.properties.push('changed');
  await Promise.resolve();
  assert.equal(settled, false);
  assert.deepEqual(calls, [{id: 8, command: 'electron-open-dialog', details: expected}]);
  const result = {canceled: false, filePaths: ['/media/one.mp4', '/media/two.mkv']};
  finish(result);
  assert.equal(await pending, result);
});

test('open preserves multiple paths and cancellation contracts', async () => {
  for (const canceled of [true, false]) {
    const selection = {canceled, filePaths: canceled ? [] : ['/one', '/two']};
    const {dialog} = create({callSync: () => selection, callAsync: () => selection});
    assert.equal(dialog.showOpenDialogSync(null, {}), canceled ? undefined : selection.filePaths);
    assert.equal(await dialog.showOpenDialog({}), selection);
  }
  const missing = create({callSync: undefined, callAsync: undefined}).dialog;
  assert.throws(() => missing.showOpenDialogSync({}), {code: 'ERR_NOT_SUPPORTED'});
  await assert.rejects(missing.showOpenDialog({}), {code: 'ERR_NOT_SUPPORTED'});
});

test('open rejects malformed filters and properties before reaching native UI', () => {
  const {dialog, calls} = create();
  for (const options of [{defaultPath: 'bad\0path'}, {filters: [{name: 'File', extensions: ['.mp4']}]},
    {filters: [{name: 'File', extensions: []}]}, {properties: 'openFile'}, {securityScopedBookmarks: 'yes'}])
    assert.throws(() => dialog.showOpenDialog(options), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls.length, 0);
});

// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const {fileURLToPath, pathToFileURL} = require('node:url');
const vm = require('node:vm');
// Validate the real composed entry, where page.js is inside its IIFE. The
// fragment intentionally contains return statements for sandbox initialization.
const page = readBootstrapPart('xenon_ipc_renderer_bootstrap.js');
const start = page.indexOf('    function upgradeWebView(el)');
const end = page.indexOf("    if (typeof Document !== 'undefined'", start);
assert.ok(start >= 0 && end > start, 'production webview source boundaries');
const source = page.slice(start, end);
function createWebView() {
  const attributes = new Map(), attachments = [], calls = [];
  const element = {style: {}, isConnected: false,
    getAttribute: name => attributes.get(name) ?? null,
    hasAttribute: name => attributes.has(name),
    setAttribute: (name, value) => attributes.set(name, value),
    removeAttribute: name => attributes.delete(name),
    appendChild() {}, dispatchEvent() {},
  };
  const context = vm.createContext({element, console, guestElements: new Map(),
    URL, urlModule: {fileURLToPath},
    MutationObserver: class {observe() {}},
    document: {createElement: () => ({style: {}})},
    transport: {attachGuest: async (_frame, preferences) => {
      attachments.push(preferences); return 7;
    }},
    ipcRenderer: {invoke: async (...args) => calls.push(args)},
  });
  vm.runInContext(source + '\nupgradeWebView(element);', context);
  return {element, attributes, attachments, calls};
}

test('webview DOM properties reflect into the first guest attachment', async () => {
  const {element, attributes, attachments, calls} = createWebView();
  element.nodeintegration = true;
  element.disablewebsecurity = true;
  element.nodeintegrationinsubframes = true;
  element.allowpopups = true;
  element.allowpopups = false;
  element.webpreferences = 'contextIsolation=no,sandbox=no';
  const preloadPath = path.resolve(__dirname, 'fixture-preload.js');
  element.preload = pathToFileURL(preloadPath).href;
  element.partition = 'persist:fixture';
  element.src = 'http://127.0.0.1:1234/';
  assert.equal(element.nodeintegration, true);
  assert.equal(attributes.get('nodeintegration'), '');
  assert.equal(attributes.has('allowpopups'), false);
  element.isConnected = true;
  element._ensureGuestAttached();
  await element._attaching;
  assert.equal(attachments.length, 1);
  assert.equal(attachments[0].nodeIntegration, true);
  assert.equal(attachments[0].webSecurity, false);
  assert.equal(attachments[0].nodeIntegrationInSubFrames, true);
  assert.equal(attachments[0].allowPopups, false);
  assert.equal(attachments[0].contextIsolation, false);
  assert.equal(attachments[0].sandbox, false);
  assert.equal(attachments[0].partition, 'persist:fixture');
  assert.equal(attachments[0].preload, preloadPath);
  assert.equal(calls[0][2], 'loadURL');
  assert.equal(calls[0][3][0], element.src);
});

test('webview explicit webSecurity preference overrides the attribute default', async () => {
  for (const [attribute, preference, expected] of [
    [false, '', true], [true, '', false],
    [false, 'webSecurity=no', false], [true, 'webSecurity=yes', true],
  ]) {
    const {element, attachments} = createWebView();
    element.disablewebsecurity = attribute;
    element.webpreferences = preference;
    element.isConnected = true;
    element._ensureGuestAttached();
    await element._attaching;
    assert.equal(attachments[0].webSecurity, expected,
      `disablewebsecurity=${attribute}, webpreferences=${preference}`);
  }
});

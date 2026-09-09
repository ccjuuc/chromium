// Copyright 2026 The Xenon Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The built-in strict assert entry point intentionally has no file suffix.
// eslint-disable-next-line no-restricted-syntax
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import {
  createIfExprContext,
  createLaunchArgs,
  evaluateIfExpr,
  findSourceRoot,
  getArg,
  isPathInside,
  parseTargetInfo,
  resolveExecutablePath,
  resolveRequestPath,
  resolveTargetDirectories,
} from './dev_webui.js';

test('source root discovery supports tools at different nesting depths', t => {
  const rootDir = fs.mkdtempSync(
      path.join(os.tmpdir(), 'dev-webui-source-root-'));
  t.after(() => fs.rmSync(rootDir, {recursive: true, force: true}));
  fs.writeFileSync(path.join(rootDir, '.gn'), 'buildconfig = "//build/config/BUILDCONFIG.gn"');
  const directToolsDir = path.join(rootDir, 'tools');
  const productToolsDir = path.join(rootDir, 'product', 'tools');
  fs.mkdirSync(directToolsDir, {recursive: true});
  fs.mkdirSync(productToolsDir, {recursive: true});

  assert.equal(findSourceRoot(directToolsDir), rootDir);
  assert.equal(findSourceRoot(productToolsDir), rootDir);
});

test('getArg supports split and assignment forms', () => {
  assert.equal(getArg('--out', undefined, ['--out', 'out/Debug']), 'out/Debug');
  assert.equal(getArg('--out', undefined, ['--out=out/Debug']), 'out/Debug');
  assert.throws(
      () => getArg('--out', undefined, ['--out']), /requires a value/);
});

test('parseTargetInfo supports trusted and untrusted WebUI URLs', () => {
  assert.deepEqual(parseTargetInfo(['chrome://settings/privacy']), {
    host: 'settings',
    pageUrl: 'chrome://settings/privacy',
  });
  assert.equal(
      parseTargetInfo(['chrome-untrusted://print/path']).host, 'print');
});

test('request paths reject Windows separators and malformed escapes', () => {
  assert.equal(resolveRequestPath('/safe/nested/file.js?x=1'),
               'safe/nested/file.js');
  assert.equal(
      resolveRequestPath('/foo/%2e%2e%5c..%5cREADME.md'), null);
  assert.equal(resolveRequestPath('/foo/%00/file.js'), null);
  assert.equal(resolveRequestPath('/foo/%ZZ/file.js'), null);
});

test('isPathInside handles platform-specific roots', () => {
  assert.equal(
      isPathInside('C:\\src', 'C:\\src\\ui', path.win32), true);
  assert.equal(
      isPathInside('C:\\src', 'C:\\other', path.win32), false);
  assert.equal(
      isPathInside('C:\\src', 'D:\\src\\ui', path.win32), false);
  assert.equal(isPathInside('/src', '/src/ui', path.posix), true);
  assert.equal(isPathInside('/src', '/other', path.posix), false);
});

test('external source paths require an explicit generated path', () => {
  const rootDir = path.resolve('test-root');
  const outDir = path.join(rootDir, 'out', 'Default');
  const externalSource = path.resolve(rootDir, '..', 'external-ui');
  assert.throws(
      () => resolveTargetDirectories(
          'custom', ['--src', externalSource], rootDir, outDir),
      /requires an explicit --gen/);

  const layout = resolveTargetDirectories(
      'custom', ['--src', externalSource, '--gen', 'generated/custom'],
      rootDir, outDir);
  assert.equal(layout.sourceDir, externalSource);
  assert.equal(layout.genDir, path.join(rootDir, 'generated', 'custom'));
});

test('source discovery is independent of the customization directory name',
     t => {
       const rootDir = fs.mkdtempSync(
           path.join(os.tmpdir(), 'dev-webui-discovery-'));
       t.after(() => fs.rmSync(rootDir, {recursive: true, force: true}));
       const webuiRoot = path.join(
           rootDir, 'another_product', 'resources', 'webui');
       const sourceDir = path.join(webuiRoot, 'custom_page');
       const sharedDir = path.join(webuiRoot, 'shared');
       fs.mkdirSync(sourceDir, {recursive: true});
       fs.mkdirSync(sharedDir, {recursive: true});
       const outDir = path.join(rootDir, 'out', 'Default');

       const layout = resolveTargetDirectories(
           'custom-page', [], rootDir, outDir);
       assert.equal(layout.sourceDir, sourceDir);
       assert.equal(
           layout.genDir,
           path.join(
               outDir,
               'gen',
               'another_product',
               'resources',
               'webui',
               'custom_page'));
       assert.equal(layout.sharedDir, sharedDir);
     });

test('source discovery supports a webui root without a project directory',
     t => {
       const rootDir = fs.mkdtempSync(
           path.join(os.tmpdir(), 'dev-webui-root-'));
       t.after(() => fs.rmSync(rootDir, {recursive: true, force: true}));
       const sourceDir = path.join(
           rootDir, 'resources', 'webui', 'root_page');
       fs.mkdirSync(sourceDir, {recursive: true});

       const layout = resolveTargetDirectories(
           'root-page', [], rootDir, path.join(rootDir, 'out', 'Default'));
       assert.equal(layout.sourceDir, sourceDir);
     });

test('generated layout discovery detects a shared parent build target', t => {
  const rootDir = fs.mkdtempSync(
      path.join(os.tmpdir(), 'dev-webui-generated-'));
  t.after(() => fs.rmSync(rootDir, {recursive: true, force: true}));
  const relativeWebuiRoot = path.join(
      'some_product', 'resources', 'webui');
  const sourceDir = path.join(rootDir, relativeWebuiRoot, 'custom_page');
  fs.mkdirSync(sourceDir, {recursive: true});
  const outDir = path.join(rootDir, 'out', 'Default');
  const generatedWebuiRoot = path.join(
      outDir, 'gen', relativeWebuiRoot);
  fs.mkdirSync(generatedWebuiRoot, {recursive: true});
  fs.writeFileSync(path.join(generatedWebuiRoot, 'resources.grd'), '<grit/>');

  const layout = resolveTargetDirectories(
      'custom-page', [], rootDir, outDir);
  assert.equal(layout.sourceDir, sourceDir);
  assert.equal(layout.genDir, generatedWebuiRoot);
  assert.equal(layout.genPrefix, 'custom_page');
});

test('--webui-root overrides built-in host compatibility mappings', t => {
  const rootDir = fs.mkdtempSync(
      path.join(os.tmpdir(), 'dev-webui-override-'));
  t.after(() => fs.rmSync(rootDir, {recursive: true, force: true}));
  const webuiRoot = path.join(rootDir, 'product_ui');
  const sourceDir = path.join(webuiRoot, 'xenon-overlay');
  fs.mkdirSync(sourceDir, {recursive: true});

  const layout = resolveTargetDirectories(
      'xenon-overlay', ['--webui-root', webuiRoot], rootDir,
      path.join(rootDir, 'out', 'Default'));
  assert.equal(layout.sourceDir, sourceDir);
});

test('host compatibility aliases still discover a renamed project root', t => {
  const rootDir = fs.mkdtempSync(
      path.join(os.tmpdir(), 'dev-webui-alias-'));
  t.after(() => fs.rmSync(rootDir, {recursive: true, force: true}));
  const webuiRoot = path.join(
      rootDir, 'renamed_product', 'resources', 'webui');
  const sourceDir = path.join(webuiRoot, 'xenon');
  fs.mkdirSync(sourceDir, {recursive: true});
  fs.mkdirSync(path.join(webuiRoot, 'shared'), {recursive: true});
  const outDir = path.join(rootDir, 'out', 'Default');

  const layout = resolveTargetDirectories(
      'xenon-overlay', [], rootDir, outDir);
  assert.equal(layout.sourceDir, sourceDir);
  assert.equal(
      layout.genDir,
      path.join(
          outDir, 'gen', 'renamed_product', 'resources', 'webui'));
  assert.equal(layout.genPrefix, 'xenon');
  assert.equal(layout.sharedDir, path.join(webuiRoot, 'shared'));
});

test('known host aliases use their GN source and generated directories', () => {
  const rootDir = path.resolve('test-root');
  const outDir = path.join(rootDir, 'out', 'Default');
  const profilePicker = resolveTargetDirectories(
      'profile-picker', [], rootDir, outDir);
  assert.equal(
      profilePicker.sourceDir,
      path.join(rootDir, 'chrome/browser/resources/signin/profile_picker'));
  assert.equal(
      profilePicker.genDir,
      path.join(
          outDir, 'gen/chrome/browser/resources/signin/profile_picker'));
  assert.equal(profilePicker.defaultEntry, 'profile_picker.html');

  const printPreview = resolveTargetDirectories('print', [], rootDir, outDir);
  assert.equal(
      printPreview.sourceDir,
      path.join(rootDir, 'chrome/browser/resources/print_preview'));
  assert.equal(printPreview.defaultEntry, 'print_preview.html');
});

test('if-expression context follows the host platform', () => {
  const win = createIfExprContext('win32');
  const mac = createIfExprContext('darwin');
  const linux = createIfExprContext('linux');
  assert.equal(evaluateIfExpr('is_win and not is_posix', win), true);
  assert.equal(evaluateIfExpr('is_macosx and is_posix', mac), true);
  assert.equal(
      evaluateIfExpr('(not is_macosx and is_posix) or is_win', linux), true);
});

test('executable is explicit and resolved from the source root', () => {
  const rootDir = path.resolve('test-root');
  assert.equal(resolveExecutablePath([], rootDir), null);
  assert.equal(
      resolveExecutablePath(['--exe', 'out/Default/browser'], rootDir),
      path.join(rootDir, 'out', 'Default', 'browser'));
});

test('launch mode selects the supported Chromium or proxy switch', () => {
  const common = {
    host: 'settings',
    devUrl: 'http://127.0.0.1:5173',
    remoteDebuggingPort: 9222,
    pageUrl: 'chrome://settings/',
  };
  assert.deepEqual(createLaunchArgs({...common, useHttpProxy: false}), [
    '--load-webui-from-disk',
    '--remote-debugging-port=9222',
    'chrome://settings/',
  ]);
  assert.equal(
      createLaunchArgs({...common, useHttpProxy: true})[0],
      '--settings-dev-url=http://127.0.0.1:5173');
});

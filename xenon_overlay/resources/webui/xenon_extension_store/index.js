// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
import '/strings.m.js';

import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

const $ = id => document.getElementById(id);
const browserExecutable = loadTimeData.getString('browserExecutable');
const suggestedSourceRoot = loadTimeData.getString('suggestedSourceRoot');
// PowerShell also treats U+2018..U+201B as single-quote delimiters.
const quote = value => `'${value.replace(/['\u2018-\u201b]/g, char => char + char)}'`;
const joinPath = (root, relative) => `${root.replace(/\/+$/, '')}/${relative}`;

function absolutePath(value) {
  const path = value.trim().replaceAll('\\', '/');
  if (/[\u0000-\u001f]/.test(path)) {
    return '';
  }
  if (/^[A-Za-z]:[\\/]/.test(browserExecutable) ||
      browserExecutable.startsWith('\\\\')) {
    return /^[A-Za-z]:\//.test(path) || /^\/\/[^/]+\/[^/]+/.test(path) ?
        path : '';
  }
  return path.startsWith('/') ? path : '';
}

function setCommand(id, command, unavailableMessage) {
  $(id).textContent = command || unavailableMessage;
  $(`copy-${id}`).disabled = !command;
}

function updateCommands() {
  const root = absolutePath($('source-root').value);
  const executable = absolutePath(browserExecutable);
  const needsRoot = !root;
  $('source-root').setAttribute('aria-invalid', String(needsRoot));
  $('source-root-error').hidden = !needsRoot;
  $('source-root-error').textContent = needsRoot ?
      '请输入源码根目录的绝对路径，随后生成服务与浏览器启动命令。' : '';
  const missingRoot = '请先填写源码根目录的绝对路径。';
  const dataDir = root ? joinPath(root, 'out/xl_extension_store') : '';
  const serveCommand = root ? [
    '& node', quote(joinPath(root, 'tools/xl_extension_store/store.mjs')),
    quote('serve'), quote('--data-dir'), quote(dataDir),
  ].join(' ') : '';
  setCommand('serve-command', serveCommand, missingRoot);

  const browserCommand = root && executable ? [
    `& ${quote(executable)}`,
    quote(`--user-data-dir=${joinPath(dataDir, 'browser-profile')}`),
    quote('--proxy-bypass-list=xl.extensions.test.com'),
    quote('--disable-xenon-sidebar-auto-hide'),
    quote('chrome://xenon-extension-store/'),
  ].join(' ') : '';
  setCommand('browser-command', browserCommand, needsRoot ? missingRoot :
      '无法取得当前浏览器的绝对路径。');

  const executableDir = executable.slice(0, executable.lastIndexOf('/') + 1);
  const testName = /\.exe$/i.test(executable) ?
      'xl_extension_store_browsertests.exe' : 'xl_extension_store_browsertests';
  const nativeTestCommand = executableDir ? [
    `& ${quote(joinPath(executableDir, testName))}`,
    quote('--gtest_filter=XlExtensionStoreBrowserTest.*'),
    quote('--test-launcher-jobs=1'),
    quote('--lang=en-US'),
  ].join(' ') : '';
  setCommand('native-test-command', nativeTestCommand,
      '无法取得当前浏览器的绝对路径。');
}

$('source-root').value = suggestedSourceRoot;
$('browser-executable').textContent = browserExecutable || '未取得可执行文件路径';
$('source-root').addEventListener('input', () => {
  $('copy-status').textContent = '';
  updateCommands();
});

for (const button of document.querySelectorAll('button[data-copy]')) {
  button.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText($(button.dataset.copy).textContent);
      $('copy-status').textContent = button.dataset.copy === 'hosts-entry' ?
          'hosts 映射已复制。' : '命令已复制，可粘贴到 PowerShell 中运行。';
    } catch {
      $('copy-status').textContent = '复制失败，请选中对应文本手动复制。';
    }
  });
}

updateCommands();

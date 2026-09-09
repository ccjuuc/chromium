#!/usr/bin/env node
// Copyright 2026 The Xenon Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Incremental development server for Chromium and Xenon WebUIs.
 *
 * Features:
 * 1. Resident `tsc --watch` compilation.
 * 2. Source-to-preprocessed synchronization.
 * 3. Automatic page reload through long-polling or CDP.
 * 4. Automatic port selection and browser launch.
 *
 * Standard Chromium WebUIs use Chromium's `--load-webui-from-disk` support
 * and therefore require `load_webui_from_disk = true` and
 * `optimize_webui = false` in the selected build.
 * Xenon WebUIs with the dev-proxy hook can use `--proxy` explicitly.
 *
 * Usage:
 *   node xenon_overlay/tools/dev_webui.js [url|host]
 *       --exe <browser-executable> [--port 5173] [--out out/Release_64]
 *       [--webui-root <parent-dir>] [--src <source-dir>]
 *       [--gen <generated-dir>] [--shared <shared-dir>]
 *       [--entry <html-file>]
 *       [--user-data-dir <path>] [--remote-debugging-port 9222]
 *       [--proxy] [--no-launch]
 *
 * Examples:
 *   node xenon_overlay/tools/dev_webui.js xenon-overlay \
 *       --exe out/Release_64/xenon.exe
 *   node xenon_overlay/tools/dev_webui.js chrome://xenon-login/ \
 *       --exe out/Default/Xenon.app/Contents/MacOS/Xenon
 *   node xenon_overlay/tools/dev_webui.js xenon-overlay \
 *       --exe out/Default/xenon
 *   node xenon_overlay/tools/dev_webui.js custom-host \
 *       --webui-root product/resources/webui --exe out/Default/browser
 *   node xenon_overlay/tools/dev_webui.js xenon-overlay --no-launch
 */

import {spawn} from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const SCRIPT_PATH = fileURLToPath(import.meta.url);
const __dirname = path.dirname(SCRIPT_PATH);

export function findSourceRoot(startDirectory) {
  let directory = path.resolve(startDirectory);
  while (true) {
    if (fs.existsSync(path.join(directory, '.gn'))) {
      return directory;
    }
    const parent = path.dirname(directory);
    if (parent === directory) {
      throw new Error(
          `Cannot find the Chromium source root above ${startDirectory}`);
    }
    directory = parent;
  }
}

const ROOT_DIR = findSourceRoot(__dirname);
const rawArgs = process.argv.slice(2);

export function getArg(name, defaultValue, args = rawArgs) {
  const assignmentPrefix = `${name}=`;
  const assignment = args.find(arg => arg.startsWith(assignmentPrefix));
  if (assignment !== undefined) {
    const value = assignment.slice(assignmentPrefix.length);
    if (!value) {
      throw new Error(`${name} requires a value`);
    }
    return value;
  }

  const index = args.indexOf(name);
  if (index !== -1 && index + 1 < args.length) {
    const value = args[index + 1];
    if (!value || value.startsWith('--')) {
      throw new Error(`${name} requires a value`);
    }
    return value;
  }
  if (index !== -1) {
    throw new Error(`${name} requires a value`);
  }
  return defaultValue;
}

function findTargetArg(args = rawArgs) {
  const valueFlags = new Set([
    '--entry',
    '--exe',
    '--gen',
    '--host',
    '--out',
    '--port',
    '--remote-debugging-port',
    '--shared',
    '--src',
    '--url',
    '--user-data-dir',
    '--webui-root',
  ]);

  for (let index = 0; index < args.length; ++index) {
    const arg = args[index];
    if (valueFlags.has(arg)) {
      ++index;
      continue;
    }
    if ([...valueFlags].some(flag => arg.startsWith(`${flag}=`))) {
      continue;
    }
    if (!arg.startsWith('-')) {
      return arg;
    }
  }
  return null;
}

export function parseTargetInfo(args = rawArgs) {
  const target = getArg('--url', undefined, args) ||
      getArg('--host', undefined, args) || findTargetArg(args) ||
      'chrome://xenon-overlay/';
  const pageUrl = target.includes('://') ? target : `chrome://${target}/`;
  let host;
  try {
    host = new URL(pageUrl).hostname.toLowerCase();
  } catch (_) {
    throw new Error(`Invalid WebUI URL or host: ${target}`);
  }
  if (!/^[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?$/.test(host)) {
    throw new Error(`Invalid WebUI host: ${host}`);
  }
  return {host: host || 'xenon-overlay', pageUrl};
}

const {host: TARGET_HOST, pageUrl: TARGET_PAGE_URL} = parseTargetInfo();
const INITIAL_PORT = Number.parseInt(getArg('--port', '5173'), 10);
const OUT_DIR = path.resolve(ROOT_DIR, getArg('--out', 'out/Release_64'));
const SHOULD_LAUNCH =
    !rawArgs.includes('--no-launch') && !rawArgs.includes('-n');
const DEV_PROXY_HOSTS = new Set(['xenon-login', 'xenon-overlay']);
const USE_HTTP_PROXY =
    rawArgs.includes('--proxy') || DEV_PROXY_HOSTS.has(TARGET_HOST);
const REMOTE_DEBUGGING_PORT = Number.parseInt(
    getArg('--remote-debugging-port', '9222'), 10);

export function resolveExecutablePath(args = rawArgs, rootDir = ROOT_DIR) {
  const executableArg = getArg('--exe', undefined, args);
  return executableArg ? path.resolve(rootDir, executableArg) : null;
}

export function createLaunchArgs({
  useHttpProxy,
  host,
  devUrl,
  remoteDebuggingPort,
  pageUrl,
  userDataDir,
  rootDir = ROOT_DIR,
}) {
  const launchArgs = [
    useHttpProxy ? `--${host}-dev-url=${devUrl}` :
        '--load-webui-from-disk',
    `--remote-debugging-port=${remoteDebuggingPort}`,
    pageUrl,
  ];
  if (userDataDir) {
    launchArgs.unshift(
        `--user-data-dir=${path.resolve(rootDir, userDataDir)}`);
  }
  return launchArgs;
}

const EXECUTABLE_PATH = resolveExecutablePath();

function existingDirectory(directory) {
  try {
    return fs.statSync(directory).isDirectory();
  } catch (_) {
    return false;
  }
}

export function isPathInside(parent, candidate, pathApi = path) {
  const relative = pathApi.relative(
      pathApi.resolve(parent), pathApi.resolve(candidate));
  return relative === '' ||
      (!relative.startsWith(`..${pathApi.sep}`) && relative !== '..' &&
       !pathApi.isAbsolute(relative));
}

function resolvePathInside(parent, relativePath) {
  const candidate = path.resolve(parent, relativePath);
  return isPathInside(parent, candidate) ? candidate : null;
}

const TARGET_LAYOUTS = new Map([
  [
    'xenon-overlay',
    {
      sourceNames: ['xenon'],
      generateFromParent: true,
      genPrefix: 'xenon',
    },
  ],
  [
    'xenon-login',
    {
      sourceNames: ['login'],
      generateFromParent: true,
      genPrefix: 'login',
    },
  ],
  [
    'profile-picker',
    {
      sourcePath: 'chrome/browser/resources/signin/profile_picker',
      genPrefix: '',
      entry: 'profile_picker.html',
    },
  ],
  [
    'print',
    {
      sourcePath: 'chrome/browser/resources/print_preview',
      genPrefix: '',
      entry: 'print_preview.html',
    },
  ],
]);

const AUTO_SEARCH_SKIPPED_DIRS = new Set([
  '.git',
  'node_modules',
  'out',
  'third_party',
]);

function listChildDirectories(directory) {
  try {
    return fs.readdirSync(directory, {withFileTypes: true})
        .filter(entry => entry.isDirectory())
        .map(entry => path.join(directory, entry.name));
  } catch (_) {
    return [];
  }
}

function uniqueExistingDirectories(directories) {
  const seen = new Set();
  return directories.filter(directory => {
    if (!existingDirectory(directory)) {
      return false;
    }
    const key = process.platform === 'win32' ?
        path.resolve(directory).toLowerCase() : path.resolve(directory);
    if (seen.has(key)) {
      return false;
    }
    seen.add(key);
    return true;
  });
}

function projectSearchBases(rootDir) {
  const bases = [rootDir];
  const children = listChildDirectories(rootDir);
  bases.push(...children);
  for (const child of children) {
    if (AUTO_SEARCH_SKIPPED_DIRS.has(path.basename(child))) {
      continue;
    }
    bases.push(...listChildDirectories(child));
  }
  return bases;
}

export function findAutomaticSourceDirectory(
    host, rootDir = ROOT_DIR, aliases = []) {
  const directoryNames = [
    ...new Set([host, host.replace(/-/g, '_'), ...aliases]),
  ];
  const bases = projectSearchBases(rootDir);
  const findMatches = suffix => uniqueExistingDirectories(
      bases.flatMap(base => directoryNames.map(
          name => path.join(base, suffix, name))));

  // A product-specific resources/webui directory takes precedence over the
  // upstream-style browser/resources directory. Ambiguous matches are not
  // guessed because editing the wrong product's resources is hard to notice.
  const customMatches = findMatches(path.join('resources', 'webui'));
  const matches = customMatches.length > 0 ? customMatches :
      findMatches(path.join('browser', 'resources'));
  if (matches.length > 1) {
    throw new Error(
        `Multiple source directories match ${host}: ${matches.join(', ')}. ` +
        'Pass --src or --webui-root to select one.');
  }
  return matches[0] || null;
}

function hasGeneratedTargetMetadata(directory) {
  return [
    'build_ts_manifest.json',
    'preprocess_static_files_manifest.json',
    'resources.grd',
    'tsconfig_build_ts.json',
  ].some(name => fs.existsSync(path.join(directory, name)));
}

function inferGeneratedLayout(
    sourceDir, genOverride, rootDir, outDir, fallbackPrefix = '') {
  if (genOverride) {
    return {
      genDir: path.resolve(rootDir, genOverride),
      genPrefix: fallbackPrefix,
    };
  }
  if (!isPathInside(rootDir, sourceDir)) {
    throw new Error(
        'A source directory outside the source tree requires an explicit ' +
        '--gen path');
  }

  let sourceBase = sourceDir;
  while (isPathInside(rootDir, sourceBase)) {
    const generatedDir = path.join(
        outDir, 'gen', path.relative(rootDir, sourceBase));
    if (hasGeneratedTargetMetadata(generatedDir)) {
      return {
        genDir: generatedDir,
        genPrefix: path.relative(sourceBase, sourceDir),
      };
    }
    if (path.resolve(sourceBase) === path.resolve(rootDir)) {
      break;
    }
    sourceBase = path.dirname(sourceBase);
  }

  const generatedSourceDir = fallbackPrefix ?
      path.dirname(sourceDir) : sourceDir;
  return {
    genDir: path.join(
        outDir, 'gen', path.relative(rootDir, generatedSourceDir)),
    genPrefix: fallbackPrefix,
  };
}

function inferSharedDirectory(
    sourceDir, sharedOverride, rootDir, webuiRoot = null) {
  if (sharedOverride) {
    return path.resolve(rootDir, sharedOverride);
  }
  const candidate = webuiRoot ? path.join(webuiRoot, 'shared') :
      path.join(path.dirname(sourceDir), 'shared');
  return existingDirectory(candidate) ? candidate : null;
}

export function resolveTargetDirectories(
    host, args = rawArgs, rootDir = ROOT_DIR, outDir = OUT_DIR) {
  const sourceOverride = getArg('--src', undefined, args);
  const genOverride = getArg('--gen', undefined, args);
  const sharedOverride = getArg('--shared', undefined, args);
  const webuiRootOverride = getArg('--webui-root', undefined, args);
  const knownLayout = TARGET_LAYOUTS.get(host);
  if (sourceOverride) {
    const sourceDir = path.resolve(rootDir, sourceOverride);
    const generatedLayout = inferGeneratedLayout(
        sourceDir, genOverride, rootDir, outDir);
    return {
      sourceDir,
      ...generatedLayout,
      sharedDir: inferSharedDirectory(
          sourceDir, sharedOverride, rootDir),
    };
  }

  if (webuiRootOverride) {
    const webuiRoot = path.resolve(rootDir, webuiRootOverride);
    const directoryNames = [
      ...new Set([
        host,
        host.replace(/-/g, '_'),
        ...(knownLayout?.sourceNames || []),
      ]),
    ];
    const matches = uniqueExistingDirectories(
        directoryNames.map(name => path.join(webuiRoot, name)));
    if (matches.length === 0) {
      throw new Error(
          `No directory for ${host} found under --webui-root ${webuiRoot}`);
    }
    const sourceDir = matches[0];
    const fallbackPrefix = knownLayout?.generateFromParent ?
        knownLayout.genPrefix : '';
    const generatedLayout = inferGeneratedLayout(
        sourceDir, genOverride, rootDir, outDir, fallbackPrefix);
    return {
      sourceDir,
      ...generatedLayout,
      defaultEntry: knownLayout?.entry,
      sharedDir: inferSharedDirectory(
          sourceDir, sharedOverride, rootDir, webuiRoot),
    };
  }

  if (knownLayout) {
    const sourceDir = knownLayout.sourcePath ?
        path.join(rootDir, knownLayout.sourcePath) :
        findAutomaticSourceDirectory(
            host, rootDir, knownLayout.sourceNames || []);
    if (!sourceDir) {
      throw new Error(
          `No source directory found for ${host}. Pass --src or ` +
          '--webui-root to select one.');
    }
    const fallbackPrefix = knownLayout.generateFromParent ?
        knownLayout.genPrefix : '';
    const generatedLayout = inferGeneratedLayout(
        sourceDir, genOverride, rootDir, outDir, fallbackPrefix);
    return {
      sourceDir,
      ...generatedLayout,
      defaultEntry: knownLayout.entry,
      sharedDir: inferSharedDirectory(
          sourceDir, sharedOverride, rootDir,
          knownLayout.generateFromParent ? path.dirname(sourceDir) : null),
    };
  }

  const sourceDir = findAutomaticSourceDirectory(host, rootDir) ||
      path.join(rootDir, 'chrome/browser/resources', host);
  const generatedLayout = inferGeneratedLayout(
      sourceDir, genOverride, rootDir, outDir);
  return {
    sourceDir,
    ...generatedLayout,
    sharedDir: inferSharedDirectory(
        sourceDir, sharedOverride, rootDir),
  };
}

const {
  sourceDir: SRC_DIR,
  genDir: GEN_DIR,
  genPrefix: GEN_PREFIX,
  defaultEntry: DEFAULT_ENTRY,
  sharedDir: SHARED_DIR,
} = resolveTargetDirectories(TARGET_HOST);
const TSC_DIR = path.join(GEN_DIR, 'tsc');
const PREPROCESSED_DIR = path.join(GEN_DIR, 'preprocessed');
const TSCONFIG_PATH = path.join(GEN_DIR, 'tsconfig_build_ts.json');
const TSC_BIN = path.join(
    ROOT_DIR, 'third_party/node/node_modules/typescript/bin/tsc');

function toGeneratedRelativePath(relativePath) {
  return GEN_PREFIX ? path.join(GEN_PREFIX, relativePath) : relativePath;
}

function findEntryHtml() {
  const explicitEntry = getArg('--entry');
  const candidates = [
    explicitEntry,
    DEFAULT_ENTRY,
    `${TARGET_HOST}.html`,
    `${TARGET_HOST.replace(/-/g, '_')}.html`,
    TARGET_HOST === 'xenon-login' ? 'login.html' : null,
    'index.html',
    'app.html',
  ].filter(Boolean);

  for (const name of candidates) {
    const safeName = resolveRequestPath(`/${name}`);
    if (safeName === null) {
      if (name === explicitEntry) {
        throw new Error(`--entry must be a relative resource path: ${name}`);
      }
      continue;
    }
    if (findExistingFileInside(SRC_DIR, safeName) ||
        findExistingFileInside(
            PREPROCESSED_DIR, toGeneratedRelativePath(safeName))) {
      return safeName;
    }
  }
  return resolveRequestPath(`/${candidates[0]}`);
}

const ENTRY_HTML = findEntryHtml();
const pendingWatchers = new Set();
let reloadTimer = null;
let cdpReady = false;
let cdpWarningShown = false;

function targetMatchesPage(targetUrl) {
  if (targetUrl === TARGET_PAGE_URL) {
    return true;
  }
  try {
    const page = new URL(TARGET_PAGE_URL);
    const target = new URL(targetUrl);
    return target.protocol === page.protocol &&
        target.hostname === page.hostname;
  } catch (_) {
    return false;
  }
}

function sendCdpReload(webSocketDebuggerUrl) {
  if (typeof globalThis.WebSocket !== 'function') {
    return Promise.reject(new Error(
        'this Node.js version does not provide a WebSocket client'));
  }
  return new Promise((resolve, reject) => {
    const socket = new globalThis.WebSocket(webSocketDebuggerUrl);
    let settled = false;
    const closeSocket = () => {
      try {
        socket.close();
      } catch (_) {
      }
    };
    const finish = error => {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(timer);
      closeSocket();
      error ? reject(error) : resolve();
    };
    const timer = setTimeout(() => {
      finish(new Error('CDP reload timed out'));
    }, 2000);
    socket.addEventListener('open', () => {
      socket.send(JSON.stringify({
        id: 1,
        method: 'Page.reload',
        params: {ignoreCache: true},
      }));
    });
    socket.addEventListener('message', event => {
      try {
        const message = JSON.parse(String(event.data));
        if (message.id === 1) {
          finish(message.error ? new Error(message.error.message) : null);
        }
      } catch (_) {
      }
    });
    socket.addEventListener('error', () => {
      finish(new Error('CDP WebSocket connection failed'));
    }, {once: true});
  });
}

async function reloadDevPage() {
  const endpoint = `http://127.0.0.1:${REMOTE_DEBUGGING_PORT}`;
  const targets = await requestJson(`${endpoint}/json/list`);
  const target = targets.find(candidate =>
    candidate.type === 'page' && targetMatchesPage(candidate.url));
  if (!target?.webSocketDebuggerUrl) {
    throw new Error(`no CDP page found for ${TARGET_PAGE_URL}`);
  }
  await sendCdpReload(target.webSocketDebuggerUrl);
}

function broadcastReload() {
  if (!USE_HTTP_PROXY) {
    if (!SHOULD_LAUNCH || !cdpReady) {
      return;
    }
    void reloadDevPage().then(() => {
      console.info(`[DevWebUI:${TARGET_HOST}] Reloaded page through CDP.`);
      cdpWarningShown = false;
    }).catch(error => {
      if (!cdpWarningShown) {
        console.warn(
            `[DevWebUI:${TARGET_HOST}] CDP reload failed: ${error.message}`);
        cdpWarningShown = true;
      }
    });
    return;
  }
  if (pendingWatchers.size === 0) {
    return;
  }
  console.info(
      `[DevWebUI:${TARGET_HOST}] Reloading ${pendingWatchers.size} client(s)...`);
  for (const response of pendingWatchers) {
    try {
      response.setHeader('Access-Control-Allow-Origin', '*');
      response.end('reload');
    } catch (_) {
    }
  }
  pendingWatchers.clear();
}

function scheduleReload(delay = 100) {
  if (reloadTimer) {
    clearTimeout(reloadTimer);
  }
  reloadTimer = setTimeout(broadcastReload, delay);
}

export function createIfExprContext(platform = process.platform) {
  const isWin = platform === 'win32';
  const isMac = platform === 'darwin';
  const isLinux = platform === 'linux';
  return {
    _google_chrome: false,
    _is_chrome_for_testing_branded: false,
    chrome_root_store_cert_management_ui: true,
    enable_dice_support: true,
    enable_extensions_core: true,
    enable_glic: false,
    enable_pdf_ink2: true,
    enable_pdf_save_to_drive: true,
    enable_webui_contextual_tasks_composebox: true,
    enable_xenon_ai: true,
    enable_xenon_service: true,
    is_android: false,
    is_chromeos: false,
    is_desktop_android: false,
    is_ios: false,
    is_linux: isLinux,
    is_macosx: isMac,
    is_official_build: false,
    is_posix: !isWin,
    is_win: isWin,
    webnn_enable_graph_dump: false,
  };
}

const IF_EXPR_CONTEXT = createIfExprContext();

export function evaluateIfExpr(expression, context = IF_EXPR_CONTEXT) {
  const tokens = expression.match(/\(|\)|\b(?:and|or|not)\b|[A-Za-z_]\w*/g) ||
      [];
  if (tokens.join('') !== expression.replace(/\s+/g, '')) {
    throw new Error(`Unsupported <if> expression: ${expression}`);
  }

  let index = 0;
  const parsePrimary = () => {
    const token = tokens[index++];
    if (token === '(') {
      const value = parseOr();
      if (tokens[index++] !== ')') {
        throw new Error(`Unbalanced <if> expression: ${expression}`);
      }
      return value;
    }
    if (!token || token === ')' || token === 'and' || token === 'or') {
      throw new Error(`Invalid <if> expression: ${expression}`);
    }
    return Boolean(context[token]);
  };
  const parseUnary = () => {
    if (tokens[index] === 'not') {
      ++index;
      return !parseUnary();
    }
    return parsePrimary();
  };
  const parseAnd = () => {
    let value = parseUnary();
    while (tokens[index] === 'and') {
      ++index;
      const right = parseUnary();
      value = value && right;
    }
    return value;
  };
  const parseOr = () => {
    let value = parseAnd();
    while (tokens[index] === 'or') {
      ++index;
      const right = parseAnd();
      value = value || right;
    }
    return value;
  };

  const value = parseOr();
  if (index !== tokens.length) {
    throw new Error(`Invalid <if> expression: ${expression}`);
  }
  return value;
}

function processIfExpr(content) {
  content = content.replace(
      /\/\/\s*<if expr="([^"]+)">([\s\S]*?)\/\/\s*<\/if>/g,
      (_, expr, block) => evaluateIfExpr(expr) ? block : '');
  return content.replace(
      /<if expr="([^"]+)">([\s\S]*?)<\/if>/g,
      (_, expr, block) => evaluateIfExpr(expr) ? block : '');
}

function syncSourceFile(relativePath) {
  try {
    const sourceFile = path.join(SRC_DIR, relativePath);
    if (!fs.existsSync(sourceFile) || fs.statSync(sourceFile).isDirectory()) {
      return;
    }

    const generatedRelativePath = toGeneratedRelativePath(relativePath);
    const targetFile = path.join(PREPROCESSED_DIR, generatedRelativePath);
    fs.mkdirSync(path.dirname(targetFile), {recursive: true});

    let content = fs.readFileSync(sourceFile, 'utf8');
    if (content.includes('<if expr')) {
      content = processIfExpr(content);
    }

    if (relativePath.endsWith('.css')) {
      const wrapperPath = `${targetFile}.ts`;
      if (fs.existsSync(wrapperPath)) {
        let wrapper = fs.readFileSync(wrapperPath, 'utf8');
        wrapper = wrapper.replace(/css`[\s\S]*?`\]\);/, () =>
          `css\`${content.replace(/`/g, '\\`').replace(/\$/g, '\\$')}\`]);`);
        fs.writeFileSync(wrapperPath, wrapper, 'utf8');
      }
      fs.writeFileSync(targetFile, content, 'utf8');
      return;
    }

    if (relativePath.endsWith('.html') && relativePath !== ENTRY_HTML) {
      const wrapperPath = `${targetFile}.ts`;
      if (fs.existsSync(wrapperPath)) {
        const escaped = content.replace(/`/g, '\\`').replace(/\$\{/g, '\\${');
        let wrapper = fs.readFileSync(wrapperPath, 'utf8');
        if (wrapper.includes('<!--_html_template_start_-->')) {
          wrapper = wrapper.replace(
              /(<!--_html_template_start_-->)[\s\S]*?(<!--_html_template_end_-->)/,
              (_, start, end) => `${start}${escaped}${end}`);
        } else if (wrapper.includes('return html`')) {
          wrapper = wrapper.replace(
              /(return\s+html`)[\s\S]*?(`;)/,
              (_, start, end) =>
                `${start}<!--_html_template_start_-->${escaped}` +
                `<!--_html_template_end_-->${end}`);
        }
        fs.writeFileSync(wrapperPath, wrapper, 'utf8');
      }
    }

    fs.writeFileSync(targetFile, content, 'utf8');
  } catch (error) {
    console.warn(
        `[DevWebUI:${TARGET_HOST}] Sync failed for ${relativePath}: ` +
        error.message);
  }
}

function startTscWatcher() {
  if (!fs.existsSync(TSCONFIG_PATH)) {
    console.warn(
        `[DevWebUI:${TARGET_HOST}] Missing ${TSCONFIG_PATH}. ` +
        'Run autoninja once first.');
    return null;
  }

  console.info(`[DevWebUI:${TARGET_HOST}] Starting resident tsc...`);
  const child = spawn(
      process.execPath,
      [TSC_BIN, '-p', TSCONFIG_PATH, '--watch', '--preserveWatchOutput'],
      {cwd: ROOT_DIR, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true});

  child.stdout.on('data', data => {
    const output = data.toString();
    if (output.includes('Watching for file changes') ||
        output.includes('Found 0 errors')) {
      console.info(
          `[DevWebUI:${TARGET_HOST}] ` + output.trim().split('\n').pop());
      scheduleReload();
    } else if (output.includes('error TS')) {
      console.error(`[DevWebUI:${TARGET_HOST}] TypeScript errors:\n${output}`);
    }
  });
  child.stderr.on('data', data => {
    console.error(`[DevWebUI:${TARGET_HOST}] tsc: ${data}`);
  });
  child.on('error', error => {
    console.error(`[DevWebUI:${TARGET_HOST}] Cannot launch tsc:`, error);
  });
  return child;
}

function startFileWatcher() {
  if (!existingDirectory(SRC_DIR)) {
    console.error(`[DevWebUI:${TARGET_HOST}] Source directory missing: ${SRC_DIR}`);
    return null;
  }

  console.info(`[DevWebUI:${TARGET_HOST}] Watching ${SRC_DIR}`);
  let debounceTimer = null;
  try {
    const watcher = fs.watch(
        SRC_DIR, {recursive: true}, (_eventType, filename) => {
          if (!filename || filename.endsWith('~') || filename.startsWith('.')) {
            return;
          }
          if (debounceTimer) {
            clearTimeout(debounceTimer);
          }
          debounceTimer = setTimeout(() => {
            console.info(`[DevWebUI:${TARGET_HOST}] Changed: ${filename}`);
            syncSourceFile(filename);
            if (!/\.tsx?$/.test(filename)) {
              scheduleReload(150);
            }
          }, 50);
        });
    watcher.on('error', error => {
      console.warn(`[DevWebUI:${TARGET_HOST}] Watcher: ${error.message}`);
    });
    return watcher;
  } catch (error) {
    console.warn(`[DevWebUI:${TARGET_HOST}] Watcher failed: ${error.message}`);
    return null;
  }
}

const MIME_MAP = {
  '.avif': 'image/avif',
  '.css': 'text/css; charset=utf-8',
  '.gif': 'image/gif',
  '.html': 'text/html; charset=utf-8',
  '.ico': 'image/x-icon',
  '.jpeg': 'image/jpeg',
  '.jpg': 'image/jpeg',
  '.js': 'application/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.map': 'application/json; charset=utf-8',
  '.mjs': 'application/javascript; charset=utf-8',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.wasm': 'application/wasm',
  '.webp': 'image/webp',
};

function getLiveReloadScript(serverPort) {
  return `
<!-- DEV AUTO LIVE RELOAD -->
<script>
(() => {
  async function listenReload() {
    while (true) {
      try {
        const response = await fetch(
            'http://127.0.0.1:${serverPort}/dev-watch?t=' + Date.now());
        if (await response.text() === 'reload') {
          location.reload();
          return;
        }
      } catch (_) {
        await new Promise(resolve => setTimeout(resolve, 1000));
      }
    }
  }
  listenReload();
})();
</script>`;
}

export function resolveRequestPath(requestUrl) {
  try {
    const url = new URL(requestUrl, 'http://127.0.0.1');
    const pathname = decodeURIComponent(url.pathname);
    if (pathname.includes('\\') || pathname.includes('\0')) {
      return null;
    }
    const segments = pathname.split('/');
    if (segments.includes('..')) {
      return null;
    }
    return segments.filter(segment => segment && segment !== '.').join('/');
  } catch (_) {
    return null;
  }
}

function findExistingFileInside(baseDir, relativePath) {
  if (!existingDirectory(baseDir)) {
    return null;
  }
  const candidate = resolvePathInside(baseDir, relativePath);
  if (!candidate || !fs.existsSync(candidate) ||
      !fs.statSync(candidate).isFile()) {
    return null;
  }

  try {
    const realBaseDir = fs.realpathSync(baseDir);
    const realCandidate = fs.realpathSync(candidate);
    return isPathInside(realBaseDir, realCandidate) ? realCandidate : null;
  } catch (_) {
    return null;
  }
}

function findResourceFile(relativePath) {
  const generatedPath = toGeneratedRelativePath(relativePath);
  const candidates = [
    [TSC_DIR, generatedPath],
    [SRC_DIR, relativePath],
    [PREPROCESSED_DIR, generatedPath],
  ];

  if (relativePath.startsWith('shared/') && SHARED_DIR) {
    const sharedPath = relativePath.slice('shared/'.length);
    candidates.push([SHARED_DIR, sharedPath]);
    if (isPathInside(ROOT_DIR, SHARED_DIR)) {
      candidates.push([
        path.join(OUT_DIR, 'gen', path.relative(ROOT_DIR, SHARED_DIR)),
        sharedPath,
      ]);
    }
  }

  candidates.push(
      [path.join(OUT_DIR, 'gen'), relativePath],
      [path.join(OUT_DIR, 'gen/chrome/browser/resources'), relativePath],
      [path.join(ROOT_DIR, 'chrome/browser/resources'), relativePath]);
  for (const [baseDir, candidatePath] of candidates) {
    const candidate = findExistingFileInside(baseDir, candidatePath);
    if (candidate) {
      return candidate;
    }
  }
  return undefined;
}

function createHttpServer(serverPort) {
  return http.createServer((request, response) => {
    if (request.url.split('?')[0] === '/dev-watch') {
      response.setHeader('Access-Control-Allow-Origin', '*');
      response.setHeader('Cache-Control', 'no-cache, no-store');
      response.setHeader('Content-Type', 'text/plain; charset=utf-8');
      pendingWatchers.add(response);
      request.on('close', () => pendingWatchers.delete(response));
      setTimeout(() => {
        if (pendingWatchers.delete(response)) {
          try {
            response.end('keep-alive');
          } catch (_) {
          }
        }
      }, 25000);
      return;
    }

    let relativePath = resolveRequestPath(request.url);
    if (relativePath === null) {
      response.writeHead(400).end('Bad Request');
      return;
    }
    if (relativePath === '' || relativePath === 'index.html') {
      relativePath = ENTRY_HTML;
    }

    const targetFile = findResourceFile(relativePath);
    if (!targetFile) {
      response.statusCode = 404;
      response.setHeader('Content-Type', 'text/plain; charset=utf-8');
      response.end(`Not Found: /${relativePath}`);
      return;
    }

    const extension = path.extname(targetFile).toLowerCase();
    response.setHeader(
        'Content-Type', MIME_MAP[extension] || 'application/octet-stream');
    response.setHeader('Access-Control-Allow-Origin', '*');
    response.setHeader('Cache-Control', 'no-cache, no-store, must-revalidate');

    if (extension === '.html') {
      let html = fs.readFileSync(targetFile, 'utf8');
      const reloadScript = getLiveReloadScript(serverPort);
      html = html.includes('</body>') ?
          html.replace('</body>', `${reloadScript}\n</body>`) :
          `${html}\n${reloadScript}`;
      response.end(html);
      return;
    }

    const stream = fs.createReadStream(targetFile);
    stream.on('error', error => {
      if (!response.headersSent) {
        response.statusCode = 500;
      }
      response.end(`Read Error: ${error.message}`);
    });
    stream.pipe(response);
  });
}

function requestJson(url, method = 'GET') {
  return new Promise((resolve, reject) => {
    const request = http.request(url, {method}, response => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', chunk => body += chunk);
      response.on('end', () => {
        if (response.statusCode < 200 || response.statusCode >= 300) {
          reject(new Error(`HTTP ${response.statusCode}: ${body}`));
          return;
        }
        try {
          resolve(JSON.parse(body));
        } catch (error) {
          reject(error);
        }
      });
    });
    request.on('error', reject);
    request.setTimeout(1000, () => request.destroy(new Error('Request timed out')));
    request.end();
  });
}

function delay(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds));
}

async function ensureDevPageOpen(debugPort) {
  const endpoint = `http://127.0.0.1:${debugPort}`;
  for (let attempt = 0; attempt < 40; ++attempt) {
    try {
      const targets = await requestJson(`${endpoint}/json/list`);
      if (targets.some(target => targetMatchesPage(target.url))) {
        cdpReady = true;
        return;
      }
      await requestJson(
          `${endpoint}/json/new?${encodeURIComponent(TARGET_PAGE_URL)}`, 'PUT');
      cdpReady = true;
      console.info(`[DevWebUI] Opened ${TARGET_PAGE_URL} through CDP.`);
      return;
    } catch (_) {
      await delay(250);
    }
  }
  console.warn(
      `[DevWebUI] CDP ${debugPort} not ready; open ${TARGET_PAGE_URL} manually.`);
}

function listenAvailablePort(port) {
  const server = createHttpServer(port);
  server.on('error', error => {
    if (error.code === 'EADDRINUSE') {
      if (port >= 65535) {
        console.error('[DevWebUI] No available TCP port found.');
        process.exitCode = 1;
        return;
      }
      console.warn(`[DevWebUI:${TARGET_HOST}] Port ${port} busy; trying ${port + 1}.`);
      listenAvailablePort(port + 1);
    } else {
      console.error(`[DevWebUI:${TARGET_HOST}] Server:`, error);
    }
  });

  server.listen(port, '127.0.0.1', () => {
    const devUrl = `http://127.0.0.1:${port}`;
    console.info('====================================================');
    console.info(`[DevWebUI] Host       : ${TARGET_HOST}`);
    console.info(`[DevWebUI] Source     : ${SRC_DIR}`);
    console.info(`[DevWebUI] Generated  : ${GEN_DIR}`);
    console.info(`[DevWebUI] Server     : ${devUrl}`);
    console.info(`[DevWebUI] Page       : ${TARGET_PAGE_URL}`);
    const mode = USE_HTTP_PROXY ? 'HTTP proxy' : 'disk + CDP';
    console.info(`[DevWebUI] Mode       : ${mode}`);
    console.info('[DevWebUI] Live reload: enabled');
    console.info('====================================================');

    startFileWatcher();
    const tscProcess = startTscWatcher();
    if (!SHOULD_LAUNCH) {
      return;
    }

    const launchArgs = createLaunchArgs({
      useHttpProxy: USE_HTTP_PROXY,
      host: TARGET_HOST,
      devUrl,
      remoteDebuggingPort: REMOTE_DEBUGGING_PORT,
      pageUrl: TARGET_PAGE_URL,
      userDataDir: getArg('--user-data-dir'),
    });
    console.info(
        `[DevWebUI] Launching ${EXECUTABLE_PATH} ` +
        `(CDP ${REMOTE_DEBUGGING_PORT})...`);
    const browser = spawn(
        EXECUTABLE_PATH, launchArgs,
        {stdio: 'ignore', windowsHide: process.platform === 'win32'});
    void ensureDevPageOpen(REMOTE_DEBUGGING_PORT);
    browser.once('error', error => {
      console.error('[DevWebUI] Browser launch failed:', error.message);
      tscProcess?.kill();
      process.exit(1);
    });
    browser.once('exit', (code, signal) => {
      const exitCode = Number.isInteger(code) ? code : 1;
      console.info(
          `[DevWebUI] Browser exited (code=${code}, signal=${signal}).`);
      tscProcess?.kill();
      process.exit(exitCode);
    });
  });
}

export function main() {
  if (!Number.isInteger(INITIAL_PORT) ||
      INITIAL_PORT < 1 || INITIAL_PORT > 65535) {
    console.error(`[DevWebUI] Invalid port: ${getArg('--port', '5173')}`);
    process.exitCode = 1;
    return;
  }
  if (!Number.isInteger(REMOTE_DEBUGGING_PORT) ||
      REMOTE_DEBUGGING_PORT < 1 || REMOTE_DEBUGGING_PORT > 65535) {
    console.error(
        '[DevWebUI] Invalid remote debugging port: ' +
        getArg('--remote-debugging-port', '9222'));
    process.exitCode = 1;
    return;
  }
  if (SHOULD_LAUNCH && !EXECUTABLE_PATH) {
    console.error(
        '[DevWebUI] --exe <browser-executable> is required unless ' +
        '--no-launch is used.');
    process.exitCode = 1;
    return;
  }
  if (SHOULD_LAUNCH &&
      (!fs.existsSync(EXECUTABLE_PATH) ||
       !fs.statSync(EXECUTABLE_PATH).isFile())) {
    console.error(
        `[DevWebUI] Browser executable not found: ${EXECUTABLE_PATH}`);
    process.exitCode = 1;
    return;
  }
  if (SHOULD_LAUNCH && process.platform !== 'win32') {
    try {
      fs.accessSync(EXECUTABLE_PATH, fs.constants.X_OK);
    } catch (_) {
      console.error(
          `[DevWebUI] Browser is not executable: ${EXECUTABLE_PATH}`);
      process.exitCode = 1;
      return;
    }
  }
  if (SHOULD_LAUNCH && !USE_HTTP_PROXY) {
    const buildflagsPath = path.join(
        OUT_DIR, 'gen/content/public/common/buildflags.h');
    const diskLoadingEnabled = fs.existsSync(buildflagsPath) &&
        /BUILDFLAG_INTERNAL_LOAD_WEBUI_FROM_DISK\(\)\s+\(1\)/.test(
            fs.readFileSync(buildflagsPath, 'utf8'));
    if (!diskLoadingEnabled) {
      console.error(
          '[DevWebUI] This WebUI uses disk mode. Set ' +
          '`load_webui_from_disk = true` in the selected build and rebuild ' +
          'the browser first.');
      process.exitCode = 1;
      return;
    }
    const generatedGrd = path.join(GEN_DIR, 'resources.grd');
    if (!fs.existsSync(generatedGrd)) {
      console.error(
          `[DevWebUI] Missing ${generatedGrd}. Build this WebUI once first.`);
      process.exitCode = 1;
      return;
    }
    if (/[\\/]minified[\\/]/.test(fs.readFileSync(generatedGrd, 'utf8'))) {
      console.error(
          '[DevWebUI] Disk mode requires `optimize_webui = false` in the ' +
          'selected build. Rebuild the browser after changing the GN arg.');
      process.exitCode = 1;
      return;
    }
  }

  listenAvailablePort(INITIAL_PORT);
}

if (process.argv[1] &&
    path.resolve(process.argv[1]) === path.resolve(SCRIPT_PATH)) {
  main();
}

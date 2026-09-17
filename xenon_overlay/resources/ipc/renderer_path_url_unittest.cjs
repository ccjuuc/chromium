// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run with: node --test xenon_overlay/resources/ipc/renderer_path_url_unittest.cjs
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const nodeUrl = require('node:url');
const test = require('node:test');
const vm = require('node:vm');
const source = readFileSync(path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js'), 'utf8');

function renderer(platform, options = {}) {
  const windows = platform === 'win32';
  const nativePath = windows ? path.win32 : path.posix;
  const root = options.root || (windows ? 'D:\\fixture\\App' : '/srv/fixture/App');
  const cwd = options.cwd || nativePath.join(root, 'working');
  const files = new Map(options.files || []);
  const aliases = new Map(options.aliases || []);
  const calls = [];
  const key = filename => windows ? nativePath.normalize(filename).toLowerCase() : nativePath.normalize(filename);
  function entry(requested) {
    let filename = nativePath.normalize(requested);
    for (const [alias, target] of aliases) {
      if (key(filename) === key(alias) || key(filename).startsWith(key(alias) + nativePath.sep)) {
        filename = target + filename.slice(alias.length);
        break;
      }
    }
    for (const [file, contents] of files) {
      if (key(file) === key(filename)) return {filename: file, contents, isFile: true, isDirectory: false};
      if (key(file).startsWith(key(filename).replace(/[\\/]$/, '') + nativePath.sep)) {
        return {filename, isFile: false, isDirectory: true};
      }
    }
    if (key(filename) === key(root)) return {filename: root, isFile: false, isDirectory: true};
    throw Object.assign(new Error('ENOENT: missing fixture path'), {code: 'ENOENT'});
  }
  const context = vm.createContext({
    __xenonPaths: {platform, arch: 'x64', endianness: 'LE', cwd},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: root, exeDir: root, cwd,
        execPath: nativePath.join(root, windows ? 'host.exe' : 'host'),
        documentPath: nativePath.join(root, 'index.html'), ...options.config}),
      setDispatchHandler() {},
      sendSync(channel, request) {
        assert.equal(channel, '__xenon:fs');
        calls.push({...request});
        const found = entry(request.path);
        if (request.operation === 'realpath') return found.filename;
        if (request.operation === 'stat') return found;
        if (request.operation === 'access') return undefined;
        if (request.operation === 'read_file') return Buffer.from(found.contents).toString('base64');
        throw new Error('Unexpected fixture fs operation: ' + request.operation);
      },
    },
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    location: {protocol: 'chrome:', hostname: 'fixture', pathname: '/index.html', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(source, context);
  return {context, root, cwd, calls, files, aliases};
}

function plain(value) { return JSON.parse(JSON.stringify(value)); }
function withCwd(cwd, fn) {
  const original = process.cwd;
  process.cwd = () => cwd;
  try { return fn(); } finally { process.cwd = original; }
}

test('both path implementations match Node for roots, trailing separators and lexical components', () => {
  const context = renderer('win32').context;
  const cases = ['', '.', '..', '...', 'a', 'a/b/', '/a//b/../c/', '/a/./b.txt',
    'a\\b', '\\a', 'C:', 'C:foo', 'C:..\\foo', 'C:/foo/../bar/', 'C:\\',
    '\\\\server\\share', '\\\\server\\share\\', '\\\\server\\share\\..\\foo',
    '//server/share/file.ext', '///a///b//', '.env', '..hidden', 'a/.', 'a/..',
    '/a/foo.tar.gz/', 'C:/a/.file', 'a//b', '/', '/..', '.\\C:',
    '\\\\?\\C:\\foo', '\\\\?\\UNC\\server\\share\\a',
    '\\\\.\\PHYSICALDRIVE0', '\\\\.\\PHYSICALDRIVE0\\..\\x'];
  for (const kind of ['win32', 'posix']) {
    const actual = context.require('path/' + kind), expected = path[kind];
    for (const filename of cases) {
      for (const method of ['normalize', 'isAbsolute', 'dirname', 'basename', 'extname', 'parse']) {
        assert.deepEqual(plain(actual[method](filename)), plain(expected[method](filename)),
            `${kind}.${method}(${JSON.stringify(filename)})`);
      }
    }
    for (const parts of [[], ['', ''], ['/', '/a', 'b'], ['//', 'host', 'share'],
      ['//host', 'share'], ['C:', 'a'], ['a', '../b/'], ['/a/', '/b']]) {
      assert.equal(actual.join(...parts), expected.join(...parts), `${kind}.join(${JSON.stringify(parts)})`);
    }
    for (const filename of ['', 'foo', '/foo', '/foo/', 'file.txt', 'C:foo', 'C:/foo/']) {
      for (const suffix of ['', 'foo', '.txt', 'txt', 'o']) {
        assert.equal(actual.basename(filename, suffix), expected.basename(filename, suffix));
      }
    }
    for (const value of [{root: '/', name: 'a', ext: 'txt'}, {dir: 'a/', base: 'b'},
      {root: 'C:\\', name: 'a', ext: '.json'}, {}]) {
      assert.equal(actual.format(value), expected.format(value));
    }
    for (const method of ['normalize', 'isAbsolute', 'dirname', 'basename', 'extname', 'parse', 'join', 'resolve']) {
      assert.throws(() => actual[method](null), {code: 'ERR_INVALID_ARG_TYPE'});
    }
  }
});

test('resolve uses the current process cwd, drive context and UNC roots', () => {
  for (const platform of ['win32', 'linux']) {
    const {context, cwd} = renderer(platform);
    const actual = context.require('path');
    const expected = platform === 'win32' ? path.win32 : path.posix;
    for (const args of [[], [''], ['.'], ['relative'], ['a', '../b'], ['a/', '../../c'],
      ...(platform === 'win32' ? [['D:child'], ['C:child'], ['\\child'],
        ['C:\\first', 'D:child'], ['D:\\first', 'D:child'],
        ['\\\\server\\share\\base', '..', 'child'], ['D:\\a', '\\b'],
        ['\\\\.\\PHYSICALDRIVE0\\..\\x'], ['\\\\?\\C:\\foo']]
        : [['/tmp', '../x'], ['/'], ['a\\b'], ['/srv/Case', 'File']])]) {
      assert.equal(actual.resolve(...args), withCwd(cwd, () => expected.resolve(...args)),
          `${platform}.resolve(${JSON.stringify(args)})`);
    }
    context.process.cwd = () => platform === 'win32' ? 'E:\\changed' : '/changed';
    assert.equal(actual.resolve('next'), platform === 'win32' ? 'E:\\changed\\next' : '/changed/next');
    assert.equal(context.require('path/posix').resolve('next'), '/changed/next');
    if (platform === 'win32') {
      context.process.env['=C:'] = 'C:\\drive-current';
      assert.equal(actual.resolve('C:child'), path.win32.resolve('C:\\drive-current', 'C:child'));
    }
  }
});

test('relative paths require real cwd metadata while absolute paths stay usable', () => {
  const {context} = renderer('linux', {config: {cwd: undefined}});
  const actual = context.require('path');
  assert.throws(() => actual.resolve('relative'), {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(actual.resolve('/declared/root', 'child'), '/declared/root/child');
});

test('relative and namespace paths match Node without losing root identity', () => {
  const {context, cwd} = renderer('win32');
  for (const kind of ['win32', 'posix']) {
    const actual = context.require('path/' + kind), expected = path[kind];
    for (const [from, to] of [['D:\\a', 'D:\\b'], ['D:\\a', 'E:\\b'],
      ['\\\\server\\share\\a', '\\\\server\\share\\b'], ['/a', '/a/b'],
      ['/a', '/'], ['a', 'b'], ['same', 'same']]) {
      assert.equal(actual.relative(from, to), withCwd(cwd, () => expected.relative(from, to)));
    }
    for (const filename of ['D:\\a', '\\\\server\\share\\a', '', '/a']) {
      assert.equal(actual.toNamespacedPath(filename), withCwd(cwd, () => expected.toNamespacedPath(filename)));
    }
  }
});

test('legacy url.parse preserves relative identity and Node field/query semantics', () => {
  const actual = renderer('linux').context.require('url');
  for (const input of ['', '/relative', 'relative?x=1&x=2#hash', '//host/a',
    'a#b?c', 'a b?x=hello world', 'http://user:pass@example.com:80/a?x=1#b',
    'http://a', 'HTTP://Example.COM/a', 'http:foo', 'file:///tmp/a%23b',
    'mailto:user@example.com', 'foo:bar/baz', 'https://[::1]:443/a',
    '/%zz?bad=%zz', 'https://例子.test/汉字']) {
    for (const parseQuery of [false, true]) for (const slashes of [false, true]) {
      const parsed = actual.parse(input, parseQuery, slashes);
      assert.deepEqual(plain(parsed), plain(nodeUrl.parse(input, parseQuery, slashes)), input);
      assert.equal(actual.format(parsed), nodeUrl.format(nodeUrl.parse(input, parseQuery, slashes)));
    }
  }
  assert.equal(actual.parse('/relative').host, null);
  assert.equal(actual.parse('/relative').href, '/relative');
  assert.throws(() => actual.parse(null), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => actual.parse('http://%zz@host'), {name: 'URIError'});
  for (const [from, to] of [['https://example.test/a/b', '../c?x=1#h'],
    ['/a/b', '../c'], ['a/b', 'c'], ['/a/b', '#hash'], ['/a/b?x=1', '?y=2'],
    ['https://example.test/a', '//other.test/b']]) {
    assert.equal(actual.resolve(from, to), nodeUrl.resolve(from, to));
  }
});

test('file URLs round trip reserved characters, unicode, spaces, UNC and platform separators', () => {
  for (const platform of ['win32', 'linux']) {
    const {context, cwd} = renderer(platform);
    const actual = context.require('url');
    const windows = platform === 'win32';
    const filenames = windows ? ['D:\\name#?% space\\汉字.txt', '\\\\server\\share\\file#?%.txt',
      'D:\\space at end ', 'D:\\a\n\tb', 'D:\\a\\', 'relative #?%.txt', 'D:child',
      '\\\\例子\\share\\a#b', '\\\\?\\D:\\file#?.txt', '\\\\?\\UNC\\server\\share\\file'] :
      ['/tmp/name#?% space/汉字.txt', '/tmp/literal\\backslash', '/tmp/a\n\tb',
        '/tmp/trailing/', 'relative #?%.txt', '/tmp/:@!$&\'()+,;=.txt'];
    for (const filename of filenames) {
      const expected = withCwd(cwd, () => nodeUrl.pathToFileURL(filename, {windows}));
      const converted = actual.pathToFileURL(filename);
      assert.equal(converted.href, expected.href, filename);
      assert.equal(actual.fileURLToPath(converted), nodeUrl.fileURLToPath(expected, {windows}));
      assert.equal(actual.fileURLToPath(converted.href), nodeUrl.fileURLToPath(expected, {windows}));
      assert.equal(converted.search, '');
      assert.equal(converted.hash, '');
    }
    for (const input of ['file:///D:/a%23b%3Fc%25d', 'file://server/share/file',
      'file://localhost/D:/a', 'file:///D:/a?ignored=1#ignored', 'file:///tmp/%zz',
      'file:///D:/a%2Fb', 'file:///D:/a%5Cb', 'https://example.test/a', '/relative',
      'file:///tmp/no-drive', 'file:///D:/%FF', 'file://xn--fsqu00a/share/a',
      'file://%zz/share/a', 'file://user:pass@host/a', 123]) {
      let value, error;
      try { value = nodeUrl.fileURLToPath(input, {windows}); } catch (e) { error = e; }
      if (error) {
        assert.throws(() => actual.fileURLToPath(input), e =>
          e.name === error.name && e.code === error.code, String(input));
      } else assert.equal(actual.fileURLToPath(input), value, String(input));
    }
    assert.equal(actual.pathToFileURL('/tmp/a#b', {windows: false}).href, 'file:///tmp/a%23b');
    assert.equal(actual.fileURLToPath('file:///tmp/a%23b', {windows: false}), '/tmp/a#b');
    assert.equal(actual.fileURLToPath('file:///D:/a%23b', {windows: true}), 'D:\\a#b');
    assert.throws(() => actual.pathToFileURL(null), {code: 'ERR_INVALID_ARG_TYPE'});
    if (windows) {
      assert.throws(() => actual.pathToFileURL('\\\\server'), {code: 'ERR_INVALID_ARG_VALUE'});
    }
  }
});

test('POSIX filesystem calls preserve case and literal backslashes; chrome uses its synthetic mount', () => {
  const filename = '/srv/fixture/App/Upper\\Name#?.txt';
  const harness = renderer('linux', {files: [[filename, 'fixture bytes']]});
  const fs = harness.context.require('fs');
  assert.equal(fs.readFileSync(filename, 'utf8'), 'fixture bytes');
  assert.equal(harness.calls.at(-1).path, filename);
  const fileUrl = harness.context.require('url').pathToFileURL(filename);
  assert.equal(fs.readFileSync(fileUrl, 'utf8'), 'fixture bytes');
  assert.equal(harness.calls.at(-1).path, filename);
  assert.throws(() => fs.readFileSync(filename.toLowerCase(), 'utf8'), {code: 'ENOENT'});
  const count = harness.calls.length;
  assert.ok(fs.statSync('chrome://fixture/').isDirectory());
  assert.equal(JSON.parse(fs.readFileSync('chrome://fixture/package.json', 'utf8')).version, '1.0.0');
  fs.mkdirSync('chrome://fixture/sub');
  fs.writeFileSync('chrome://fixture/sub/file.txt', 'virtual');
  assert.equal(fs.readFileSync('chrome://fixture/sub/file.txt', 'utf8'), 'virtual');
  assert.deepEqual(Array.from(fs.readdirSync('chrome://fixture/sub')), ['file.txt']);
  assert.equal(harness.calls.length, count);
});

test('POSIX module aliases and invalidated caches retain canonical roots and case boundaries', () => {
  const root = '/srv/fixture/App';
  const filename = root + '/Entry.js';
  const harness = renderer('linux', {files: [
    [filename, 'module.exports = {ready: true};'],
    [root + '/entry.js', 'module.exports = {lower: true};'],
    ['/srv/fixture/app/private.js', 'throw new Error("must not read");'],
    [root + '-other/private.js', 'throw new Error("must not read");'],
  ], aliases: [[root + '/alias.js', filename]]});
  const require = harness.context.require;
  const first = require('./Entry.js');
  assert.equal(require('./alias.js'), first);
  assert.notEqual(require('./entry.js'), first);
  assert.equal(require.resolve('./alias.js'), filename);
  for (const outside of ['/srv/fixture/app/private.js', root + '-other/private.js']) {
    assert.throws(() => require(outside), {code: 'MODULE_NOT_FOUND'});
  }
  delete require.cache[filename];
  harness.aliases.set(root + '/alias.js', '/srv/fixture/app/private.js');
  assert.throws(() => require('./alias.js'), {code: 'MODULE_NOT_FOUND'});
  assert.ok(!harness.calls.some(call => call.operation === 'read_file' && call.path.includes('private')));
});

test('POSIX mapped source names load from declared roots and filesystem root remains permitted', () => {
  for (const root of ['/srv/fixture/App', '/srv/fixture/App/', '/']) {
    const filename = path.posix.join(root, 'entry.js');
    const {context} = renderer('linux', {root, files: [[filename, 'module.exports = 42;']],
      config: {rendererUrlMappings: [{sourcePathPrefix: root, targetBaseUrl: 'chrome://fixture/'}]}});
    assert.equal(context.require('./entry.js', 'chrome://fixture/renderer.js'), 42);
    assert.equal(context.require('./entry.js', nodeUrl.pathToFileURL(
        path.posix.join(root, 'renderer#?.js'), {windows: false}).href), 42);
  }
});

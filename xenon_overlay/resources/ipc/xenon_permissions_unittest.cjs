const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function runtime() {
  const calls = [], errors = [];
  const contents = {id: 10, isDestroyed: () => false};
  const context = vm.createContext({
    console: {error: (...args) => errors.push(args)},
    BrowserWindow: {fromId: id => id === 3 ? {webContents: contents} : null},
    guestWebContents: new Map(), webContentsState: new WeakMap(),
    WebFrameMain: class {constructor(fields) {Object.assign(this, fields);}},
    electronUnsupported: name => {throw Object.assign(new Error(name), {code: 'ERR_NOT_SUPPORTED'});},
    __xenonBrowserWindowCall: (id, command, request) => {
      assert.equal(id, 0); assert.equal(command, 'electron-permissions');
      calls.push(structuredClone(request));
    },
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'main/permissions.js'), 'utf8'), context);
  return {context, calls, errors, contents};
}

function request(registration, extra = {}) {
  return {partition: '', registration, requestId: 7, kind: 'request',
    webContentsId: 3, permission: 'notifications', requestingOrigin: 'https://example.test/',
    details: {requestingUrl: 'https://example.test/page', isMainFrame: true}, ...extra};
}

test('permission registration rolls back native failure and preserves partition', () => {
  const {context, calls} = runtime();
  context.setSessionPermissionHandler('persist:test', 'check', () => true);
  const registration = calls[0].registration;
  assert.equal(calls[0].partition, 'persist:test');
  context.__xenonBrowserWindowCall = () => {throw new Error('native rejected');};
  assert.throws(() => context.setSessionPermissionHandler('persist:test', 'check', () => false), /native rejected/);
  assert.equal(context.__xenonCheckPermission(request(registration, {partition: 'persist:test'})), true);
  assert.throws(() => context.setSessionPermissionHandler('', 'check', {}), /function or null/);
});

test('new closure is available to a synchronous check during native registration', () => {
  const {context} = runtime();
  let observed;
  context.__xenonBrowserWindowCall = (_id, _command, args) => {
    observed = context.__xenonCheckPermission(request(args.registration));
  };
  context.setSessionPermissionHandler('', 'check', () => true);
  assert.equal(observed, true);
});

test('checks execute real synchronous closures with the owned WebContents and current details', () => {
  const {context, calls, contents} = runtime();
  let allow = false;
  context.setSessionPermissionHandler('', 'check', (sender, permission, origin, details) => {
    assert.equal(sender, contents); assert.equal(permission, 'notifications');
    assert.equal(origin, 'https://example.test/'); assert.equal(details.isMainFrame, true);
    return allow;
  });
  const input = request(calls[0].registration);
  assert.equal(context.__xenonCheckPermission(input), false);
  allow = true;
  assert.equal(context.__xenonCheckPermission(input), true);
  context.setSessionPermissionHandler('', 'check', async () => true);
  assert.throws(() => context.__xenonCheckPermission(request(calls.at(-1).registration)), /synchronously/);
  context.setSessionPermissionHandler('', 'check', null);
  assert.throws(() => context.__xenonCheckPermission(input), {code: 'ERR_ABORTED'});
});

test('asynchronous request callbacks respond once and retain request registration', () => {
  const {context, calls} = runtime();
  let callback;
  context.setSessionPermissionHandler('', 'request', (_sender, _permission, reply) => {callback = reply;});
  const registration = calls[0].registration;
  context.dispatchElectronPermissionRequest(request(registration));
  assert.equal(calls.length, 1);
  callback(true); callback(false);
  assert.equal(calls.length, 2);
  assert.deepEqual(calls[1], {method: 'response', partition: '', kind: 'request', registration, requestId: 7, granted: true});
});

test('throws and stale registrations deny rather than fabricating permission', () => {
  const {context, calls, errors} = runtime();
  context.setSessionPermissionHandler('', 'request', () => {throw new Error('failed handler');});
  context.dispatchElectronPermissionRequest(request(calls[0].registration));
  assert.equal(calls.at(-1).granted, false);
  context.dispatchElectronPermissionRequest(request(999));
  assert.equal(calls.at(-1).granted, false);
  assert.equal(errors.length, 2);
});

test('display callbacks serialize actual source identity without thumbnails or forged frame objects', () => {
  const {context, calls} = runtime();
  context.setSessionPermissionHandler('', 'display', (details, reply) => {
    assert.equal(details.securityOrigin, 'https://example.test');
    reply({video: {id: 'window:123:0', name: 'Fixture', thumbnail: {mustNotSerialize: true}}, audio: 'loopback'});
  });
  context.dispatchElectronPermissionRequest(request(calls[0].registration, {
    kind: 'display', details: {securityOrigin: 'https://example.test', frame: {processId: 12, routingId: 34}},
  }));
  assert.deepEqual(calls.at(-1).selection, {video: {id: 'window:123:0', name: 'Fixture'}, audio: 'loopback'});
  assert.throws(() => context.setSessionPermissionHandler('', 'display', () => {}, {useSystemPicker: true}), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => context.normalizeDisplaySelection({video: {routingId: 3}}), {code: 'ERR_NOT_SUPPORTED'});
});

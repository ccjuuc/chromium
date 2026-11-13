const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');

function runtime() {
  const requests = [];
  const native = new Map();
  let next = 1;
  let failure;
  const factory = vm.runInNewContext(fs.readFileSync(path.join(__dirname,
      'main/views.js'), 'utf8') + ';createElectronViews;', {WeakRef, FinalizationRegistry});
  const types = factory({EventEmitter,
    isReady: () => true,
    callNative(request) {
      requests.push(request);
      if (failure) throw failure;
      if (request.action === 'root' || request.action === 'create') {
        const id = next++;
        native.set(id, {bounds: {x: 0, y: 0, width: 0, height: 0}, visible: true});
        return {id, webContentsId: -id};
      }
      const entry = native.get(request.id);
      if (request.action === 'get-bounds') return {...entry.bounds};
      if (request.action === 'set-bounds') entry.bounds = {...request.bounds};
      if (request.action === 'get-visible') return entry.visible;
      if (request.action === 'set-visible') entry.visible = request.visible;
      return null;
    },
    preparePreferences(value) { return {contextIsolation: true, nodeIntegration: false, ...value}; },
    createContents(id, preferences) { return {nativeId: id, preferences}; },
    getContentsNativeId(value) { if (!value || !(value.nativeId < 0)) throw new TypeError('Expected WebContents'); return value.nativeId; },
    getWindowId(window) { return window.id; },
  });
  return {...types, requests, native, fail(value) { failure = value; }};
}

test('View tree reparenting and reordering preserve single ownership', () => {
  const env = runtime();
  const one = new env.View(), two = new env.View(), a = new env.View(), b = new env.View();
  one.addChildView(a); one.addChildView(b); one.addChildView(a, 1);
  assert.deepEqual(Array.from(one.children), [b, a]);
  two.addChildView(a);
  assert.deepEqual(Array.from(one.children), [b]);
  assert.deepEqual(Array.from(two.children), [a]);
  two.removeChildView(a);
  assert.equal(two.children.length, 0);
  one.addChildView(a);
  assert.deepEqual(Array.from(one.children), [b, a]);
});

test('View rejects cycles and invalid operations before native mutation', () => {
  const env = runtime();
  const root = new env.View(), child = new env.View();
  root.addChildView(child);
  const before = env.requests.length;
  assert.throws(() => child.addChildView(root), /ancestors/);
  assert.throws(() => child.addChildView(child), /itself/);
  assert.throws(() => root.addChildView({}), /Expected a View/);
  assert.throws(() => root.addChildView(child, -1), /index/);
  assert.throws(() => child.setBounds({x: 0, y: 0, width: -1, height: 10}), /negative/);
  assert.throws(() => child.setVisible('false'), /boolean/);
  assert.equal(env.requests.length, before);
});

test('Native rejection leaves the JavaScript View graph unchanged', () => {
  const env = runtime();
  const one = new env.View(), two = new env.View(), child = new env.View();
  one.addChildView(child);
  env.fail(new Error('Owner was revoked'));
  assert.throws(() => two.addChildView(child), /revoked/);
  assert.deepEqual(Array.from(one.children), [child]);
  assert.equal(two.children.length, 0);
  assert.throws(() => one.removeChildView(child), /revoked/);
  assert.deepEqual(Array.from(one.children), [child]);
});

test('View reads actual native geometry and visibility instead of cached values', () => {
  const env = runtime(), view = new env.View();
  env.native.get(1).bounds = {x: 1, y: 2, width: 720, height: 480};
  assert.deepEqual(view.getBounds(), {x: 1, y: 2, width: 720, height: 480});
  env.native.get(1).visible = false;
  assert.equal(view.getVisible(), false);
  view.setBounds({x: 4, y: 8, width: 40, height: 80});
  assert.deepEqual(view.getBounds(), {x: 4, y: 8, width: 40, height: 80});
});

test('BrowserWindow contentView is lazy, stable and unusable after close', () => {
  const env = runtime();
  let destroyed = false;
  const window = {id: 5, isDestroyed: () => destroyed};
  assert.equal(env.requests.length, 0);
  const root = env.contentView(window);
  assert.equal(env.contentView(window), root);
  assert.equal(env.requests.length, 1);
  assert.equal(env.requests[0].windowId, 5);
  assert.equal(root.children.length, 0);
  destroyed = true;
  assert.throws(() => env.contentView(window), /destroyed/);
});

test('WebContentsView creates a real native identity and preserves adopted WebContents', () => {
  const env = runtime();
  const view = new env.WebContentsView({webPreferences: {partition: 'persist:test'}});
  assert.ok(view instanceof env.View);
  assert.equal(view.webContents.nativeId, -1);
  assert.equal(view.webContents.preferences.partition, 'persist:test');
  assert.equal(view.webContents.preferences.contextIsolation, true);
  assert.equal(view.webContents.preferences.nodeIntegration, false);
  const adopted = new env.WebContentsView({webContents: view.webContents});
  assert.equal(adopted.webContents, view.webContents);
  assert.equal(env.requests.at(-1).webContentsId, -1);
  assert.throws(() => new env.WebContentsView({webContents: {}}), /Expected WebContents/);
  assert.throws(() => view.setBorderRadius(-1), /radius/);
  view.setBorderRadius(8);
  assert.equal(env.requests.at(-1).action, 'set-border-radius');
});

test('Native View events target the corresponding live wrapper', () => {
  const env = runtime(), first = new env.View(), second = new env.View();
  const events = [];
  first.on('bounds-changed', () => events.push('first'));
  second.on('bounds-changed', () => events.push('second'));
  env.dispatch({id: 2, event: 'bounds-changed'});
  env.dispatch({id: 999, event: 'bounds-changed'});
  assert.deepEqual(events, ['second']);
});

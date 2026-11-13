// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const plain = value => JSON.parse(JSON.stringify(value));

function fixture() {
  const context = vm.createContext({Buffer});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'common/native_image.js'), 'utf8') + '\n' +
    fs.readFileSync(path.join(__dirname, 'main/notification.js'), 'utf8'), context);
  const calls = [], events = [], png = Buffer.from([137,80,78,71,13,10,26,10]);
  let ready = true, supported = true, failure;
  const nativeImage = context.createNativeImageApi({
    readFileSync: () => png,
    callSync: () => ({pngBase64:png.toString('base64'),width:1,height:1}),
  });
  const Notification = context.createNotificationClass({EventEmitter, nativeImage,
    isReady: () => ready, host: context,
    callHost: request => {
      calls.push(plain(request));
      if (failure) throw failure;
      if (request.method === 'isSupported') return supported;
    },
  });
  return {Notification, nativeImage, calls, events, png,
    ready: value => { ready = value; }, supported: value => { supported = value; },
    fail: value => { failure = value; },
    dispatch: (id, event, args = []) => context.__xenonDispatchNotificationEvent({id,event,args}),
    shutdown: () => context.__xenonShutdownNotifications(),
  };
}

test('Notification construction waits for app readiness and support queries the native host', () => {
  const f = fixture();
  f.ready(false);
  assert.throws(() => new f.Notification(), {code:'ERR_NOT_READY'});
  assert.equal(f.Notification.isSupported(), true);
  f.supported(false);
  assert.equal(f.Notification.isSupported(), false);
  f.ready(true);
  const notice = new f.Notification();
  assert.ok(notice instanceof EventEmitter);
  assert.deepEqual(f.calls, [{method:'isSupported'},{method:'isSupported'}]);
  assert.equal(notice.title, '');
  assert.equal(notice.urgency, 'low');
  assert.equal(notice.timeoutType, 'default');
});

test('show snapshots native options and delivers only actual broker events for the current display', () => {
  const f = fixture(), events = [];
  const notice = new f.Notification({title:'First',body:'Body',silent:true,
    timeoutType:'never',urgency:'critical',icon:'local-icon.png'});
  for (const name of ['show','click','close','failed'])
    notice.on(name, (event, error) => events.push([name,event.sender === notice,error]));
  assert.equal(notice.show(), undefined);
  assert.deepEqual(events, []);
  const first = f.calls[0];
  assert.deepEqual(first, {method:'show',id:1,options:{title:'First',body:'Body',silent:true,
    timeoutType:'never',urgency:'critical',iconPng:f.png.toString('base64')}});
  notice.title = 'Second';
  assert.equal(first.options.title, 'First');
  f.dispatch(1, 'show'); f.dispatch(1, 'click');
  notice.show();
  assert.deepEqual(f.calls[1], {method:'close',id:1});
  assert.equal(f.calls[2].id, 2);
  assert.equal(f.calls[2].options.title, 'Second');
  f.dispatch(1, 'show'); f.dispatch(1, 'close'); f.dispatch(1, 'failed', ['stale']);
  f.dispatch(2, 'show'); f.dispatch(2, 'close'); f.dispatch(2, 'close');
  assert.deepEqual(events, [['show',true,undefined],['click',true,undefined],
    ['show',true,undefined],['close',true,undefined]]);
});

test('programmatic close is idempotent and removes callbacks without inventing OS close events', () => {
  const f = fixture(), notice = new f.Notification(), events = [];
  for (const name of ['show','click','close']) notice.on(name, () => events.push(name));
  notice.close(); assert.deepEqual(f.calls, []);
  notice.show(); notice.close(); notice.close();
  assert.deepEqual(f.calls.map(call => call.method), ['show','close']);
  assert.deepEqual(events, []);
  f.dispatch(1, 'show'); f.dispatch(1, 'click');
  assert.deepEqual(events, []);
  f.dispatch(1, 'close');
  assert.deepEqual(events, []);
  notice.close();
  notice.show();
  f.dispatch(2, 'show'); f.dispatch(2, 'close');
  assert.deepEqual(events, ['show','close']);
});

test('native failures are forwarded and failed synchronous submissions release their display identity', () => {
  const f = fixture(), notice = new f.Notification(), errors = [];
  notice.on('failed', (event, error) => errors.push([event.sender === notice,error]));
  const failure = Object.assign(Error('host unavailable'), {code:'ERR_NOT_SUPPORTED'});
  f.fail(failure);
  assert.throws(() => notice.show(), failure);
  f.dispatch(1, 'failed', ['late']);
  assert.deepEqual(errors, []);
  f.fail(undefined);
  notice.show();
  f.dispatch(2, 'failed', ['native display failed']);
  f.dispatch(2, 'failed', ['duplicate']);
  assert.deepEqual(errors, [[true,'native display failed']]);
  notice.show();
  assert.equal(f.calls.at(-1).id, 3);
});

test('unsupported notification options fail explicitly while default platform values remain valid', () => {
  const f = fixture();
  for (const options of [{subtitle:'text'},{hasReply:true},{actions:[{type:'button',text:'Go'}]},
    {replyPlaceholder:'Reply'},{sound:'Ping'},{closeButtonText:'Done'},{toastXml:'<toast/>'}])
    assert.throws(() => new f.Notification(options), {code:'ERR_NOT_SUPPORTED'});
  for (const options of [{title:42},{silent:'false'},{actions:{}},null,[]])
    assert.throws(() => new f.Notification(options), {code:'ERR_INVALID_ARG_TYPE'});
  for (const options of [{urgency:'maximum'},{timeoutType:'minute'}])
    assert.throws(() => new f.Notification(options), {code:'ERR_INVALID_ARG_VALUE'});
  const notice = new f.Notification({subtitle:'',hasReply:false,actions:[],sound:'',
    replyPlaceholder:'',closeButtonText:'',toastXml:'',
    get applicationMetadata() { throw Error('unrelated option must not be read'); }});
  assert.throws(() => { notice.hasReply = true; }, {code:'ERR_NOT_SUPPORTED'});
  assert.throws(() => { notice.body = null; }, {code:'ERR_INVALID_ARG_TYPE'});
  const actions = notice.actions; actions.push({type:'button'});
  assert.deepEqual(Array.from(notice.actions), []);
  assert.deepEqual(f.calls, []);
});

test('icons use the NativeImage brand and empty images do not send fake image bytes', () => {
  const f = fixture();
  assert.throws(() => new f.Notification({icon:{toPNG(){throw Error('must not call');}}}),
    {code:'ERR_INVALID_ARG_TYPE'});
  const empty = new f.Notification({icon:f.nativeImage.createEmpty()});
  empty.show();
  assert.equal(Object.hasOwn(f.calls[0].options, 'iconPng'), false);
  const image = f.nativeImage.createFromBuffer(f.png);
  image.toPNG = () => { throw Error('untrusted method override'); };
  new f.Notification({icon:image}).show();
  assert.equal(f.calls[1].options.iconPng, f.png.toString('base64'));
});

test('shutdown closes outstanding notifications once and drops late native events', () => {
  const f = fixture(), notice = new f.Notification(), events = [];
  notice.on('close', () => events.push('close'));
  notice.show();
  f.shutdown(); f.shutdown(); f.dispatch(1,'close');
  assert.deepEqual(f.calls.map(call => call.method), ['show','close']);
  assert.deepEqual(events, []);
  assert.throws(() => notice.show(), {code:'ERR_INVALID_STATE'});
});

test('main bootstrap exports Notification and routes native events without creating a BrowserWindow', () => {
  const calls = [];
  const context = vm.createContext({TextEncoder,TextDecoder,URL,URLSearchParams,queueMicrotask,
    setTimeout,clearTimeout,setInterval,clearInterval,atob,btoa,performance,
    __xenonPlatform:'win32',__xenonArch:'x64',__xenonEndianness:'LE',__xenonOsRelease:'10',
    __xenonAppPath:'C:\\fixture',__xenonRendererBaseUrl:'',__xenonRendererUrlMappings:[],
    __xenonAppName:'fixture',__xenonAppVersion:'1',__xenonUserAgent:'',__xenonExecPath:process.execPath,
    __xenonPid:process.pid,__xenonEnv:{},__xenonChromeVersion:'153',__xenonV8Version:'',__xenonGetPath:()=>'',
    __xenonBrowserWindowCall: (id, command, request) => {
      assert.equal(id, 0); assert.equal(command, 'notification');
      calls.push(plain(request));
      if (request.method === 'isSupported') return true;
    },
  });
  vm.runInContext(readBootstrap(path.join(__dirname, 'xenon_ipc_main_bootstrap.js')), context);
  const {Notification, BrowserWindow} = context.__xenonElectron;
  assert.equal(Notification.isSupported(), true);
  assert.throws(() => new Notification(), {code:'ERR_NOT_READY'});
  context.__xenonMarkAppReady();
  const notice = new Notification({title:'Desktop',body:'Native delivery'}), shown = [];
  notice.on('show', event => shown.push(event.sender === notice));
  notice.show();
  assert.deepEqual(shown, []);
  assert.equal(BrowserWindow.getAllWindows().length, 0);
  context.__xenonDispatchBrowserWindowEvent(0,'notification',{id:1,event:'show',args:[]});
  assert.deepEqual(shown, [true]);
  context.__xenonShutdownApp();
  assert.deepEqual(calls.map(call => call.method), ['isSupported','show','close']);
});

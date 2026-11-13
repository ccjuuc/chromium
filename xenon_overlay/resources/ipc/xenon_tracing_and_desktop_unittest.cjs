// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function factories() {
  const context = vm.createContext({Buffer, structuredClone});
  for (const name of ['content_tracing', 'desktop_capturer'])
    vm.runInContext(fs.readFileSync(path.join(__dirname, 'main', name + '.js'), 'utf8'), context);
  return context;
}
const plain = value => JSON.parse(JSON.stringify(value));

test('tracing snapshots configuration and uses actual asynchronous host results', async () => {
  const calls = [], f = factories();
  let resolve;
  const tracing = f.createContentTracing({resolvePath: value => '/tmp/' + value,
    callHost: args => { calls.push(plain(args)); return new Promise(r => { resolve = r; }); }});
  const options = {included_categories:['v8'],memory_dump_config:{triggers:[]}};
  const pending = tracing.startRecording(options);
  options.included_categories.push('blink');
  await Promise.resolve();
  assert.deepEqual(calls, [{method:'startRecording',options:{included_categories:['v8'],memory_dump_config:{triggers:[]}}}]);
  resolve(null); assert.equal(await pending, undefined);
  const stop = tracing.stopRecording('trace.json');
  await Promise.resolve();
  assert.deepEqual(calls[1], {method:'stopRecording',path:'/tmp/trace.json'});
  resolve('/tmp/trace.json'); assert.equal(await stop, '/tmp/trace.json');
});

test('tracing rejects invalid input and retains native failures', async () => {
  const f = factories(), error = Object.assign(new Error('busy'),{code:'ERR_RESOURCE_BUSY'});
  const tracing = f.createContentTracing({resolvePath: value => value,callHost: async () => { throw error; }});
  assert.throws(() => tracing.startRecording(null), {code:'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => tracing.stopRecording('bad\0path'), {code:'ERR_INVALID_ARG_TYPE'});
  await assert.rejects(tracing.getCategories(), e => e === error);
  await assert.rejects(tracing.getTraceBufferUsage(), e => e === error);
});

test('desktop capture forwards options and returns real NativeImage objects', async () => {
  const f = factories(), calls = [], images = [];
  const capturer = f.createDesktopCapturer({isReady:()=>true,
    nativeImage:{createFromBuffer:bytes=>{const image={bytes};images.push(image);return image;},createEmpty:()=>({empty:true})},
    callHost: async options => {calls.push(plain(options));return [
      {id:'screen:2:0',name:'Desktop',display_id:'42',thumbnail:'AQID',appIcon:null},
      {id:'window:7:0',name:'Window',display_id:'',thumbnail:'',appIcon:'BAU='},
    ];},
  });
  const sources = await capturer.getSources({types:['screen','window','screen'],fetchWindowIcons:true});
  assert.deepEqual(calls,[{types:['screen','window'],width:150,height:150,fetchWindowIcons:true}]);
  assert.equal(sources[0].thumbnail,images[0]);
  assert.deepEqual(images[0].bytes,Buffer.from([1,2,3]));
  assert.equal(sources[0].display_id,'42');
  assert.equal(sources[0].appIcon,null);
  assert.equal(sources[1].thumbnail.empty,true);
  assert.equal(sources[1].appIcon,images[1]);
});

test('desktop capture validates before host access, including readiness and finite dimensions', async () => {
  const f = factories();let calls=0,ready=false;
  const capturer = f.createDesktopCapturer({isReady:()=>ready,nativeImage:{},callHost:async()=>{++calls;return [];}});
  await assert.rejects(capturer.getSources({types:['screen']}),{code:'ERR_NOT_READY'});
  ready=true;
  for(const options of [{}, {types:['tab']}, {types:['screen'],thumbnailSize:{width:Infinity,height:2}},
    {types:['screen'],thumbnailSize:{width:-1,height:1}}, {types:['screen'],fetchWindowIcons:'yes'}])
    await assert.rejects(capturer.getSources(options),{code:'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls,0);
  assert.deepEqual(plain(await capturer.getSources({types:['screen'],thumbnailSize:{width:0,height:0}})),[]);
  assert.equal(calls,1);
});

test('desktop capture preserves OS denial and never converts it to empty success', async () => {
  const f = factories(), error = Object.assign(new Error('OS denied'),{code:'ERR_ACCESS_DENIED'});
  const capturer = f.createDesktopCapturer({isReady:()=>true,nativeImage:{},callHost:async()=>{throw error;}});
  await assert.rejects(capturer.getSources({types:['screen']}),e=>e===error);
});

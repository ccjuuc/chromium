// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const realFs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const url = require('node:url');
const test = require('node:test');
const vm = require('node:vm');

function fixture(t) {
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-mkdtemp-'));
  t.after(() => realFs.rmSync(directory, {recursive:true,force:true}));
  const context = vm.createContext({TextEncoder,TextDecoder,URL,atob,btoa,
    urlModule:url, fs:{promises:{}}});
  vm.runInContext(['common/buffer_numeric.js','common/buffer.js','common/native_fs.js',
    'common/fs_mkdtemp.js'].map(file => realFs.readFileSync(path.join(__dirname,file),'utf8')).join('\n') +
    '\nglobalThis.Buffer = createBufferClass();',context);
  const calls=[];
  function perform(request) {
    calls.push({...request});
    assert.equal(request.operation,'mkdtemp');
    assert.equal(request.returnBytes,true);
    return Uint8Array.from(realFs.mkdtempSync(request.path,{encoding:'buffer'})).buffer;
  }
  context.installMkdtemp(context.fs,{Buffer:context.Buffer,path:context.nativeFsPathArgument,
    sync:perform,asynchronous:request=>Promise.resolve().then(()=>perform(request))});
  return {...context,directory,calls};
}

test('mkdtemp creates unique real directories from string, Buffer and file URL prefixes',async t=>{
  const f=fixture(t), prefix=path.join(f.directory,'临时-');
  const first=f.fs.mkdtempSync(prefix);
  const second=await f.fs.promises.mkdtemp(f.Buffer.from(prefix));
  let returned=false;
  const pending=new Promise((resolve,reject)=>f.fs.mkdtemp(new f.URL(url.pathToFileURL(prefix).href),
    (error,value)=>{try{assert.equal(returned,true);error?reject(error):resolve(value);}catch(error){reject(error)}}));
  returned=true;
  const third=await pending;
  assert.equal(new Set([first,second,third]).size,3);
  for(const created of [first,second,third]) {
    assert.equal(created.slice(0,-6),prefix);
    assert.equal(realFs.statSync(created).isDirectory(),true);
    assert.deepEqual(realFs.readdirSync(created),[]);
    if(process.platform!=='win32') assert.equal(realFs.statSync(created).mode&0o777,0o700);
  }
  const directoryPrefix=f.directory+path.sep;
  assert.equal(path.dirname(await f.fs.promises.mkdtemp(directoryPrefix)),f.directory);
});

test('mkdtemp validates and snapshots encoding before performing filesystem mutations',async t=>{
  const f=fixture(t),prefix=path.join(f.directory,'中文-');
  for(const encoding of ['buffer','utf8','hex','base64','base64url','latin1','ascii','utf16le']) {
    const output=await f.fs.promises.mkdtemp(prefix,{encoding});
    const entries=realFs.readdirSync(f.directory).sort();
    const candidates=entries.map(entry=>Buffer.from(path.join(f.directory,entry)));
    if(encoding==='buffer') {
      assert.equal(f.Buffer.isBuffer(output),true);
      assert.ok(candidates.some(bytes=>bytes.equals(Buffer.from(output))));
    } else assert.ok(candidates.some(bytes=>bytes.toString(encoding)===output));
  }
  const options={encoding:'hex'};
  const pending=f.fs.promises.mkdtemp(prefix,options);
  options.encoding='buffer';
  assert.equal(typeof await pending,'string');
  const count=f.calls.length;
  for(const bad of [23,true,{encoding:'wat'},{encoding:'BUFFER'},{encoding:{}}]) {
    let reference;try{realFs.mkdtempSync(prefix,bad)}catch(error){reference=error}
    assert.ok(reference);
    const expected={name:reference.name,code:reference.code};
    assert.throws(()=>f.fs.mkdtempSync(prefix,bad),expected);
    assert.throws(()=>f.fs.mkdtemp(prefix,bad,()=>assert.fail('invalid callback')),expected);
    await assert.rejects(f.fs.promises.mkdtemp(prefix,bad),expected);
  }
  for(const bad of [undefined,null,17,{},[]])
    assert.throws(()=>f.fs.mkdtempSync(bad),{code:'ERR_INVALID_ARG_TYPE'});
  assert.throws(()=>f.fs.mkdtempSync(prefix+'\0x'),{code:'ERR_INVALID_ARG_VALUE'});
  assert.throws(()=>f.fs.mkdtemp(prefix+'\0x',null),{code:'ERR_INVALID_ARG_TYPE'});
  assert.equal(f.calls.length,count);
});

test('mkdtemp reports the native template path and never creates missing parents',async t=>{
  const f=fixture(t),prefix=path.join(f.directory,'missing','prefix');
  const expected={code:'ENOENT',path:prefix+'XXXXXX',syscall:'mkdtemp'};
  assert.throws(()=>f.fs.mkdtempSync(prefix),expected);
  await assert.rejects(f.fs.promises.mkdtemp(prefix),expected);
  const received=await new Promise(resolve=>f.fs.mkdtemp(prefix,(...args)=>resolve(args)));
  assert.equal(received.length,1);
  assert.equal(received[0].code,expected.code);
  assert.equal(received[0].path,expected.path);
  assert.equal(realFs.existsSync(path.dirname(prefix)),false);
});

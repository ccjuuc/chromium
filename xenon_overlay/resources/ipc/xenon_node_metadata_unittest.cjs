// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const url = require('node:url');
const v8 = require('node:v8');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const bufferLimits = require('node:buffer').constants;
const certificate = fs.readFileSync(path.resolve(__dirname,
    '../../../net/data/ssl/certificates/ok_cert.pem'));

function runtime(t, kind, original = false) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-node-metadata-'));
  t.after(() => fs.rmSync(directory, {recursive:true,force:true}));
  const requests = [], marks = [];
  const perform = request => {
    requests.push({...request});
    assert.equal(request.operation, 'mkdtemp');
    assert.equal(request.noAsar, original ? true : undefined);
    return Uint8Array.from(fs.mkdtempSync(request.path, {encoding:'buffer'})).buffer;
  };
  const parse = (operation, bytes) => {
    assert.equal(operation, 'parse');
    const value = new crypto.X509Certificate(Buffer.from(bytes));
    const result = {};
    for (const key of ['subject','issuer','serialNumber','fingerprint','fingerprint256',
      'fingerprint512','validFrom','validTo','ca']) result[key] = value[key];
    return {...result, raw:value.raw, pem:value.toString(),
      validFromMilliseconds:+value.validFromDate, validToMilliseconds:+value.validToDate};
  };
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, performance,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console:{log(){},warn(){},error(){}},
    location:{protocol:'chrome:',hostname:'metadata-fixture',search:''},
    __xenonPaths:{platform:process.platform,arch:process.arch,endianness:os.endianness()},
    xenonIpcRenderer:{
      getRuntimeConfig:()=>({appPath:directory,execPath:process.execPath,exeDir:path.dirname(process.execPath), bufferLimits}),
      setDispatchHandler(){}, x509CertificateCall:parse,
      sendSync(channel,request){assert.equal(channel,'__xenon:fs');return perform(request);},
      invoke(channel,request){assert.equal(channel,'__xenon:fs');return Promise.resolve().then(()=>perform(request));},
    },
    __xenonFsCall:perform,__xenonFsCallAsync:request=>Promise.resolve().then(()=>perform(request)),
    __xenonX509CertificateCall:parse,__xenonMarkAsUncloneable:value=>marks.push(value),
    __xenonGetHeapCodeStatistics:()=>v8.getHeapCodeStatistics(),
    __xenonPlatform:process.platform,__xenonArch:process.arch,
    __xenonBufferLimits:bufferLimits,
    __xenonEndianness:os.endianness(),__xenonOsRelease:os.release(),
    __xenonAppPath:directory,__xenonRendererBaseUrl:'',__xenonRendererUrlMappings:[],
    __xenonAppName:'fixture',__xenonAppVersion:'1',__xenonUserAgent:'',
    __xenonExecPath:process.execPath,__xenonPid:process.pid,__xenonEnv:{},
    __xenonChromeVersion:'153',__xenonV8Version:'',__xenonGetPath:()=>'',
    __xenonWorkerThreadId:kind==='worker'?1:0,__xenonWorkerResourceLimits:{},
    __xenonWorkerData:null,__xenonUtilityProcess:kind==='utility',
    __xenonUtilityProcessCall:()=>false,__xenonUtilityExit(){},
    __xenonExecArgv:[],__xenonArgv:['host','fixture.js'],
  });
  const renderer = kind === 'renderer';
  vm.runInContext(readBootstrap(path.join(__dirname,
      `xenon_ipc_${renderer?'renderer':'main'}_bootstrap.js`)),context);
  const filesystem = renderer ? context.require(original?'original-fs':'fs') :
      context[original?'__xenonOriginalFs':'__xenonFs'];
  return {directory,requests,marks,context,filesystem,
    buffer:renderer?context.require('buffer'):context.__xenonBuffer,
    Buffer:renderer?context.require('buffer').Buffer:context.Buffer,
    crypto:renderer?context.require('crypto'):context.__xenonCrypto,
    http:renderer?context.require('http'):context.__xenonHttp,
    https:renderer?context.require('https'):context.__xenonHttps};
}

for (const kind of ['main','renderer','worker','utility']) {
  test(`${kind}: Buffer module exposes immutable native limits and standard aliases`, t=>{
    const {buffer} = runtime(t,kind);
    assert.equal(buffer.kMaxLength, bufferLimits.MAX_LENGTH);
    assert.equal(buffer.kStringMaxLength, bufferLimits.MAX_STRING_LENGTH);
    for (const name of ['MAX_LENGTH','MAX_STRING_LENGTH']) {
      assert.deepEqual(Object.getOwnPropertyDescriptor(buffer.constants,name),
        Object.getOwnPropertyDescriptor(bufferLimits,name));
    }
    assert.equal(Object.getOwnPropertyDescriptor(buffer,'constants').writable,false);
    assert.equal(Object.getOwnPropertyDescriptor(buffer,'constants').configurable,false);
    assert.equal(Reflect.set(buffer.constants,'MAX_STRING_LENGTH',1),false);
    buffer.kStringMaxLength=1;
    assert.equal(buffer.constants.MAX_STRING_LENGTH,bufferLimits.MAX_STRING_LENGTH);
  });

  test(`${kind}: Buffer UTF8/ASCII validators match Node bytes, subviews and detached inputs`, t=>{
    const f=runtime(t,kind),reference=require('node:buffer');
    const check=value=>{
      for(const method of ['isAscii','isUtf8']) {
        let expected;
        try { expected={value:reference[method](value)}; }
        catch(error) { expected={error:{name:error.name,code:error.code}}; }
        if(expected.error) assert.throws(()=>f.buffer[method](value),
          error=>error.name===expected.error.name&&error.code===expected.error.code);
        else assert.equal(f.buffer[method](value),expected.value,method);
      }
    };
    for(const value of [undefined,null,1,'text',[],{},new DataView(new ArrayBuffer(2)),
      Buffer.from('A中🙂'),Buffer.from([0xff]),new Uint16Array([0xffff]),
      new BigUint64Array([0n]),new ArrayBuffer(4),new SharedArrayBuffer(4),
      {[Symbol.toStringTag]:'ArrayBuffer',byteLength:0}]) check(value);
    const backing=new Uint8Array([0xff,0xe4,0xb8,0xad,0xff]);
    check(backing.subarray(1,4));check(backing.subarray(1,3));check(backing.buffer);
    const hidden=backing.subarray(1,4);
    Object.defineProperties(hidden,{buffer:{value:new ArrayBuffer(0)},byteLength:{value:0}});
    check(hidden);
    for(const data of [[0xc0,0x80],[0xc1,0xbf],[0xed,0xa0,0x80],[0xf4,0x90,0x80,0x80],
      [0xf5,0x80,0x80,0x80],[0xe0,0x9f,0xbf],[0xf0,0x8f,0xbf,0xbf],
      [0xf4,0x8f,0xbf,0xbf],[0xef,0xbf,0xbd],[0xe4,0xb8],[0x80]]) check(Uint8Array.from(data));
    const pair=new Uint8Array(2);
    for(let value=0;value<65536;++value){pair[0]=value>>>8;pair[1]=value&255;check(pair);}
    let seed=0x12345678;
    for(let i=0;i<512;++i){const data=new Uint8Array(i%17);
      for(let j=0;j<data.length;++j){seed=(Math.imul(seed,1664525)+1013904223)>>>0;data[j]=seed>>>24;}check(data);}
    const detached=new ArrayBuffer(4),view=new Uint8Array(detached),dataView=new DataView(detached);
    structuredClone(detached,{transfer:[detached]});
    check(detached);check(view);check(dataView);
  });

  test(`${kind}: HTTP validators preserve Node errors and legitimate latin1 header values`, t=>{
    const f=runtime(t,kind);
    const compare=(method,args)=>{
      let expected;
      try{expected={value:http[method](...args)};}
      catch(error){expected={error:{name:error.name,code:error.code}};}
      if(expected.error)assert.throws(()=>f.http[method](...args),
        error=>error.name===expected.error.name&&error.code===expected.error.code);
      else assert.equal(f.http[method](...args),expected.value);
    };
    for(const name of [undefined,null,1,[],{},'',Symbol('name'),'a b','a:b','中文','x-custom',"!#$%&'*+-.^_`|~"])
      compare('validateHeaderName',[name]);
    for(const name of ['x',undefined,null,Symbol('name')])
      for(const value of [undefined,null,1,[],{},Symbol('value'),'line\r\nbreak','中文','é','space\ttab','\0','\x7f',''])
        compare('validateHeaderValue',[name,value]);
    for(let byte=0;byte<256;++byte)compare('validateHeaderValue',['x',String.fromCharCode(byte)]);
    for(const method of ['validateHeaderName','validateHeaderValue'])assert.equal(Object.keys(f.http).includes(method),true);
  });

  test(`${kind}: complete bootstrap exposes real X509 metadata and standard HTTP constants`, t=>{
    const f=runtime(t,kind),reference=new crypto.X509Certificate(certificate);
    const value=new f.crypto.X509Certificate(certificate.toString());
    assert.equal(value.fingerprint256,reference.fingerprint256);
    assert.equal(value.subject,reference.subject);
    assert.equal(value.toJSON(),reference.toJSON());
    assert.equal(f.Buffer.isBuffer(value.raw),true);
    assert.equal(Object.keys(f.crypto).includes('X509Certificate'),true);
    assert.deepEqual({...f.http.STATUS_CODES},{...http.STATUS_CODES});
    assert.equal(Object.hasOwn(f.https,'STATUS_CODES'),false);
    if(kind!=='renderer') {
      assert.deepEqual(f.marks,[value]);
      const first=f.context.__xenonV8.getHeapCodeStatistics();
      const second=f.context.__xenonV8.getHeapCodeStatistics();
      assert.notEqual(first,second);
      assert.deepEqual(Object.keys(first).sort(),Object.keys(v8.getHeapCodeStatistics()).sort());
      for(const count of Object.values(first)) assert.ok(Number.isFinite(count)&&count>=0);
      assert.equal(Object.keys(f.context.__xenonV8).includes('getHeapCodeStatistics'),true);
    }
  });

  for (const original of [false,true]) {
    test(`${kind} ${original?'original-fs':'fs'}: mkdtemp preserves prefixes and promise module identity`,async t=>{
      const f=runtime(t,kind,original),prefix=f.directory+path.sep;
      const first=f.filesystem.mkdtempSync(prefix,{encoding:'buffer'});
      assert.equal(f.Buffer.isBuffer(first),true);
      const second=await f.filesystem.promises.mkdtemp(new f.context.URL(url.pathToFileURL(prefix).href));
      const third=await new Promise((resolve,reject)=>f.filesystem.mkdtemp(f.Buffer.from(prefix),
        (error,result)=>error?reject(error):resolve(result)));
      for(const created of [first.toString(),second,third]) {
        assert.equal(path.dirname(created),f.directory);
        assert.equal(fs.statSync(created).isDirectory(),true);
      }
      assert.equal(new Set([first.toString(),second,third]).size,3);
      assert.deepEqual(f.requests.map(request=>request.path),[prefix,prefix,prefix]);
      const promises=kind==='renderer'?f.context.require(original?'original-fs/promises':'fs/promises'):
        f.context[original?'__xenonOriginalFsPromises':'__xenonFsPromises'];
      assert.equal(promises,f.filesystem.promises);
      const missing=path.join(f.directory,'absent','prefix');
      await assert.rejects(promises.mkdtemp(missing),{code:'ENOENT',path:missing+'XXXXXX',syscall:'mkdtemp'});
    });
  }
}

// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
function runtime(worker = false) {
  let directory = '/initial';
  const calls = [];
  const context = vm.createContext({TextEncoder, TextDecoder, URL, URLSearchParams,
    queueMicrotask, performance, setTimeout, clearTimeout, setInterval, clearInterval,
    atob, btoa, console:{log(){},warn(){},error(){}},
    __xenonPlatform:process.platform,__xenonArch:process.arch,
    __xenonEndianness:os.endianness(),__xenonOsRelease:os.release(),
    __xenonAppPath:'/fixture',__xenonRendererBaseUrl:'',__xenonRendererUrlMappings:[],
    __xenonAppName:'fixture',__xenonAppVersion:'1',__xenonUserAgent:'',
    __xenonExecPath:process.execPath,__xenonPid:process.pid,__xenonEnv:{},
    __xenonChromeVersion:'153',__xenonV8Version:'',__xenonGetPath:()=>'',
    __xenonWorkerThreadId:worker?1:0,__xenonWorkerResourceLimits:{},
    __xenonWorkerData:null,__xenonUtilityProcess:false,
    __xenonExecArgv:[],__xenonArgv:['host','fixture.js'],
    __xenonProcessDirectory(...args) {
      calls.push(args);
      if (!args.length) return {ok:true,directory};
      if(args[0]==='missing')return {ok:false,code:'ENOENT',errno:-2,message:'no such directory'};
      if(args[0]==='embedded')return {ok:false,code:'ENOTSUP',errno:-1,message:'unsupported'};
      directory=args[0];return {ok:true};
    },
  });
  vm.runInContext(readBootstrap(path.join(__dirname,'xenon_ipc_main_bootstrap.js')),context);
  return {process:context.process,calls,context};
}
test('process cwd/chdir use native state and preserve meaningful errors',()=>{
  const f=runtime();
  assert.equal(f.process.cwd(),'/initial');
  assert.equal(f.process.chdir('/unicode/中文'),undefined);
  assert.equal(f.process.cwd(),'/unicode/中文');
  assert.throws(()=>f.process.chdir('missing'),{code:'ENOENT',errno:-2,syscall:'chdir',path:'/unicode/中文',dest:'missing'});
  assert.throws(()=>f.process.chdir('embedded'),{code:'ERR_NOT_SUPPORTED'});
  assert.equal(f.process.cwd(),'/unicode/中文');
  f.context.__xenonWorkingDirectory='/forged';
  assert.equal(f.process.cwd(),'/unicode/中文');
});
test('invalid directories never call native transport or conversion hooks',()=>{
  const f=runtime();
  for(const value of [undefined,null,1,{},new URL('file:///example')])
    assert.throws(()=>f.process.chdir(value),{code:'ERR_INVALID_ARG_TYPE'});
  assert.throws(()=>f.process.chdir({toString(){throw Error('must not convert');}}),{code:'ERR_INVALID_ARG_TYPE'});
  assert.throws(()=>f.process.chdir('bad\0path'),{code:'ERR_INVALID_ARG_VALUE'});
  assert.equal(f.calls.length,0);
});
test('worker observes current native cwd but cannot change it',()=>{
  const f=runtime(true);
  assert.equal(f.process.cwd(),'/initial');
  for(const value of [undefined,'/next'])assert.throws(()=>f.process.chdir(value),
    {name:'TypeError',code:'ERR_WORKER_UNSUPPORTED_OPERATION'});
  assert.deepEqual(f.calls,[[]]);
});

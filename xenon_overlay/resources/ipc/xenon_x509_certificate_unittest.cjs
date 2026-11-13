// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert=require('node:assert/strict');
const crypto=require('node:crypto');
const fs=require('node:fs');
const path=require('node:path');
const test=require('node:test');
const vm=require('node:vm');
const source=fs.readFileSync(path.join(__dirname,'common/x509_certificate.js'),'utf8');
const certificate=fs.readFileSync(path.resolve(__dirname,'../../../net/data/ssl/certificates/ok_cert.pem'));
const root=fs.readFileSync(path.resolve(__dirname,'../../../net/data/ssl/certificates/root_ca_cert.pem'));

function fixture() {
  const context=vm.createContext({Buffer});
  vm.runInContext(source,context);
  const calls=[],marked=[];
  const X509Certificate=context.createX509CertificateClass({Buffer,markAsUncloneable:value=>marked.push(value),call(operation,bytes,name,flags){
    calls.push({operation,name,flags});
    const value=new crypto.X509Certificate(Buffer.from(bytes));
    if(operation==='parse') {
      const fields={};
      for(const key of ['subject','issuer','serialNumber','fingerprint','fingerprint256','fingerprint512','validFrom','validTo','ca'])
        fields[key]=value[key];
      return {...fields,pem:value.toString(),raw:value.raw,
        validFromMilliseconds:+value.validFromDate,validToMilliseconds:+value.validToDate};
    }
    if(operation==='issued') return value.checkIssued(new crypto.X509Certificate(Buffer.from(name)));
    const options={subject:flags&1?'always':flags&32?'never':'default',
      wildcards:!(flags&2),partialWildcards:!(flags&4),multiLabelWildcards:!!(flags&8),singleLabelSubdomains:!!(flags&16)};
    return value[{host:'checkHost',email:'checkEmail',ip:'checkIP'}[operation]](name,options);
  }});
  return {X509Certificate,calls,marked};
}

test('X509 facade preserves real PEM/DER metadata and copies typed-array byte ranges',()=>{
  const {X509Certificate}=fixture(),reference=new crypto.X509Certificate(certificate);
  const padded=Buffer.concat([Buffer.from('prefix!'),certificate,Buffer.from('suffix!')]);
  const inputs=[certificate.toString(),certificate,new DataView(padded.buffer,padded.byteOffset+7,certificate.length)];
  for(const input of inputs) {
    const value=new X509Certificate(input);
    for(const key of ['subject','issuer','serialNumber','fingerprint','fingerprint256','fingerprint512','validFrom','validTo','ca'])
      assert.equal(value[key],reference[key]);
    assert.equal(+value.validFromDate,+reference.validFromDate);
    assert.equal(+value.validToDate,+reference.validToDate);
    assert.deepEqual(value.raw,reference.raw);
    assert.equal(value.toString(),reference.toString());
    assert.equal(JSON.stringify(value),JSON.stringify(reference));
    assert.equal(value.issuerCertificate,undefined);
    assert.equal(new X509Certificate(value.raw).fingerprint256,reference.fingerprint256);
    assert.deepEqual(Object.keys(value),[]);
  }
});

test('public raw bytes and Date mutations do not change native matching identity',()=>{
  const {X509Certificate,marked}=fixture(),bytes=Buffer.from(certificate),value=new X509Certificate(bytes);
  assert.deepEqual(marked,[value]);
  bytes.fill(0);
  assert.equal(value.checkIP('127.0.0.1'),'127.0.0.1');
  value.raw.fill(0);
  value.validFromDate.setFullYear(1970);
  assert.equal(value.checkHost('127.0.0.1'),'127.0.0.1');
  assert.equal(value.checkHost('127.0.0.1',{subject:'never'}),undefined);
  assert.equal(value.checkIssued(new X509Certificate(root)),true);
  assert.equal(value.toString().startsWith('-----BEGIN CERTIFICATE-----'),true);
});

test('X509 validates Node names/options and explicitly rejects unsupported capabilities',()=>{
  const {X509Certificate,calls}=fixture(),value=new X509Certificate(certificate);
  const reference=new crypto.X509Certificate(certificate);
  for(const options of [null,[],false,{subject:1},{subject:'invalid'},{wildcards:1},{partialWildcards:null},{multiLabelWildcards:0}]) {
    let expected;try{reference.checkHost('a',options)}catch(error){expected=error}
    assert.ok(expected);
    const count=calls.length;
    assert.throws(()=>value.checkHost('a',options),{name:expected.name,code:expected.code});
    assert.equal(calls.length,count);
  }
  for(const input of [null,17,{},certificate.buffer])
    assert.throws(()=>new X509Certificate(input),{code:'ERR_INVALID_ARG_TYPE'});
  assert.throws(()=>value.checkIP(17),{code:'ERR_INVALID_ARG_TYPE'});
  assert.throws(()=>value.checkIssued({}),{code:'ERR_INVALID_ARG_TYPE'});
  assert.throws(()=>X509Certificate.prototype.toString.call({}),{code:'ERR_INVALID_THIS'});
  for(const key of ['subjectAltName','infoAccess','keyUsage','publicKey'])
    assert.throws(()=>value[key],{code:'ERR_NOT_SUPPORTED'});
  for(const key of ['verify','checkPrivateKey','toLegacyObject'])
    assert.throws(()=>value[key](),{code:'ERR_NOT_SUPPORTED'});
});

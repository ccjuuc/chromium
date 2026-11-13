// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_x509_certificate.h"

#include <string>

#include "base/base_paths.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "gin/converter.h"
#include "gin/test/v8_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-typed-array.h"

namespace xenon::ipc {
namespace {

class XenonX509CertificateTest : public gin::V8Test {
 protected:
  v8::Isolate* isolate() { return instance_->isolate(); }
  v8::Local<v8::Context> context() { return context_.Get(isolate()); }
  void SetUp() override {
    gin::V8Test::SetUp();
    v8::HandleScope handles(isolate());
    v8::Context::Scope scope(context());
    InstallX509CertificateBridge(isolate(), context(), context()->Global(),
                                 "x509");
  }
  v8::Local<v8::Value> Run(const std::string& source) {
    return v8::Script::Compile(
               context(), gin::StringToV8(isolate(), source).As<v8::String>())
        .ToLocalChecked()
        ->Run(context())
        .ToLocalChecked();
  }
  void Load(const char* name, const char* variable) {
    base::FilePath root;
    ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
    std::string contents;
    ASSERT_TRUE(base::ReadFileToString(
        root.AppendASCII("net/data/ssl/certificates").AppendASCII(name),
        &contents));
    // Test certificates can contain a private key alongside their public PEM.
    // Only the test process reads it; no key material is printed or exported.
    auto bytes = v8::ArrayBuffer::New(isolate(), contents.size());
    UNSAFE_BUFFERS(
        base::span(static_cast<uint8_t*>(bytes->Data()), contents.size()))
        .copy_from(base::as_byte_span(contents));
    ASSERT_TRUE(context()
                    ->Global()
                    ->CreateDataProperty(
                        context(), gin::StringToV8(isolate(), variable),
                        v8::Uint8Array::New(bytes, 0, contents.size()))
                    .FromJust());
  }
};

TEST_F(XenonX509CertificateTest,
       ParsesPemAndDerWithRealMetadataAndCanonicalOutput) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  Load("ok_cert.pem", "input");
  EXPECT_TRUE(Run(R"JS(
    globalThis.cert = x509('parse', input);
    globalThis.again = x509('parse', cert.raw);
    cert.subject === 'C=US\nST=California\nL=Mountain View\nO=Test CA\nCN=127.0.0.1' &&
    cert.issuer.endsWith('CN=Test Root CA') && cert.ca === false &&
    cert.pem.startsWith('-----BEGIN CERTIFICATE-----\n') &&
    cert.pem.endsWith('\n-----END CERTIFICATE-----\n') &&
    !cert.pem.includes('PRIVATE KEY') && cert.pem === again.pem &&
    cert.fingerprint.length === 59 && cert.fingerprint256.length === 95 &&
    cert.fingerprint512.length === 191 && cert.fingerprint256 === again.fingerprint256 &&
    cert.serialNumber === again.serialNumber && cert.serialNumber.length > 0 &&
    cert.validFromMilliseconds === Date.parse(cert.validFrom) &&
    cert.validToMilliseconds === Date.parse(cert.validTo) &&
    cert.validToMilliseconds > cert.validFromMilliseconds &&
    cert.raw.length > 0 && cert.raw.length === again.raw.length &&
    cert.raw.every((byte, index) => byte === again.raw[index]);
  )JS")
                  ->IsTrue());
  EXPECT_TRUE(Run(R"JS(
    (() => {try {x509('parse', new Uint8Array([1,2,3])); return false;}
      catch (error) {return error.code === 'ERR_CRYPTO_OPERATION_FAILED';}})()
  )JS")
                  ->IsTrue());
}

TEST_F(XenonX509CertificateTest,
       MatchesNamesWildcardsAndIssuerWithoutTrustSideEffects) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  Load("ok_cert.pem", "input");
  Load("wildcard.pem", "wildcard");
  Load("root_ca_cert.pem", "root");
  EXPECT_TRUE(Run(R"JS(
    // The leaf has IP-only SANs. Node still checks its DNS common name.
    x509('host', input, '127.0.0.1', 0) === '127.0.0.1' &&
    x509('host', input, '127.0.0.1', 32) === undefined &&
    x509('ip', input, '127.0.0.1', 0) === '127.0.0.1' &&
    x509('ip', input, '127.0.0.2', 0) === undefined &&
    x509('email', input, 'missing@example.org', 0) === undefined &&
    x509('host', wildcard, 'one.example.org', 0) === '*.example.org' &&
    x509('host', wildcard, 'two.one.example.org', 0) === undefined &&
    x509('host', wildcard, 'one.example.org', 2) === undefined &&
    x509('issued', input, root) === true && x509('issued', root, input) === false &&
    x509('parse', root).ca === true
  )JS")
                  ->IsTrue());
  EXPECT_TRUE(Run(R"JS(
    (() => {
      for (const [operation,name,flags,code] of [
        ['host','bad\0name',0,'ERR_INVALID_ARG_VALUE'],
        ['ip','not-an-ip',0,'ERR_INVALID_ARG_VALUE'],
        ['host','one.example.org',1,'ERR_NOT_SUPPORTED'],
        ['host','one.example.org',8,'ERR_NOT_SUPPORTED'],
        ['host','.example.org',0,'ERR_NOT_SUPPORTED']]) {
        try {x509(operation,wildcard,name,flags); return false;}
        catch(error) {if(error.code!==code) return false;}
      }
      return true;
    })()
  )JS")
                  ->IsTrue());
}

TEST_F(XenonX509CertificateTest,
       PartialWildcardDoesNotSilentlyBecomeAMismatch) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  Load("wildcard.pem", "wildcard");
  EXPECT_TRUE(Run(R"JS(
    (() => {
      const raw=x509('parse',wildcard).raw;
      const from=Array.from('*.example.org', c=>c.charCodeAt(0));
      const to=Array.from('x*.xample.org', c=>c.charCodeAt(0));
      let replaced=false;
      for(let i=0;i<=raw.length-from.length;++i) {
        if(from.every((byte,j)=>raw[i+j]===byte)) {raw.set(to,i);replaced=true;break;}
      }
      if(!replaced) return false;
      // Altering a name invalidates its signature, but parsing and name
      // matching do not establish trust or verify that signature in Node.
      try {x509('host',raw,'xyz.xample.org',0);return false;}
      catch(error) {if(error.code!=='ERR_NOT_SUPPORTED')return false;}
      return x509('host',raw,'xyz.xample.org',4)===undefined;
    })()
  )JS")
                  ->IsTrue());
}

TEST_F(XenonX509CertificateTest,
       NonDnsCommonNameDoesNotSilentlyBecomeAMismatch) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  Load("self-signed-invalid-name.pem", "input");
  EXPECT_TRUE(Run(R"JS(
    (() => {
      if (!x509('parse',input).subject.endsWith('CN=SS B')) return false;
      try {x509('host',input,'SS B',0);return false;}
      catch(error) {if(error.code!=='ERR_NOT_SUPPORTED')return false;}
      return x509('host',input,'SS B',32)===undefined;
    })()
  )JS")
                  ->IsTrue());
}

}  // namespace
}  // namespace xenon::ipc

// Node X509Certificate facade backed by the local BoringSSL certificate parser.
function createX509CertificateClass({Buffer, call, markAsUncloneable}) {
  const states = new WeakMap();
  const invalid = (name, code = 'ERR_INVALID_ARG_TYPE') =>
    Object.assign(new TypeError('Invalid ' + name), {code});
  const unsupported = name => {
    throw Object.assign(new Error('X509Certificate.' + name + ' is not supported'),
      {code:'ERR_NOT_SUPPORTED'});
  };
  const state = value => {
    const result = states.get(value);
    if (!result) throw invalid('X509Certificate receiver', 'ERR_INVALID_THIS');
    return result;
  };
  function flags(options = {}) {
    if (options === null || typeof options !== 'object' || Array.isArray(options))
      throw invalid('options');
    const {subject = 'default', wildcards = true, partialWildcards = true,
      multiLabelWildcards = false, singleLabelSubdomains = false} = {...options};
    if (typeof subject !== 'string') throw invalid('options.subject');
    for (const [name, value] of Object.entries({wildcards,partialWildcards,multiLabelWildcards,singleLabelSubdomains}))
      if (typeof value !== 'boolean') throw invalid('options.' + name);
    if (!['default','always','never'].includes(subject))
      throw invalid('options.subject','ERR_INVALID_ARG_VALUE');
    // Explicit Node/OpenSSL flag values remain independent of BoringSSL's
    // zero-valued compatibility macros for unsupported matching options.
    return (subject === 'always' ? 1 : subject === 'never' ? 32 : 0) |
      (!wildcards ? 2 : 0) | (!partialWildcards ? 4 : 0) |
      (multiLabelWildcards ? 8 : 0) | (singleLabelSubdomains ? 16 : 0);
  }
  function check(receiver, operation, name, options) {
    if (typeof name !== 'string') throw invalid('name');
    const selected = flags(options);
    return call(operation, state(receiver).bytes, name, selected);
  }
  class X509Certificate {
    constructor(input) {
      if (typeof input === 'string') input = Buffer.from(input);
      if (!ArrayBuffer.isView(input)) throw invalid('buffer');
      // Copy the bytes of any TypedArray/DataView, not its numeric elements.
      const bytes = Buffer.from(new Uint8Array(input.buffer,input.byteOffset,input.byteLength));
      const data = call('parse',bytes);
      // A mutable public raw Buffer must never alter subsequent native checks.
      states.set(this,{data,bytes:Buffer.from(data.raw)});
      // Node's special X509 transfer hooks are not implemented here. Its Node
      // serializers must reject this value instead of silently returning {}.
      if (markAsUncloneable) markAsUncloneable(this);
    }
    get subject() { return state(this).data.subject; }
    get issuer() { return state(this).data.issuer; }
    get serialNumber() { return state(this).data.serialNumber; }
    get fingerprint() { return state(this).data.fingerprint; }
    get fingerprint256() { return state(this).data.fingerprint256; }
    get fingerprint512() { return state(this).data.fingerprint512; }
    get validFrom() { return state(this).data.validFrom; }
    get validTo() { return state(this).data.validTo; }
    get validFromDate() {
      const value=state(this);
      return value.fromDate ||= new Date(value.data.validFromMilliseconds);
    }
    get validToDate() {
      const value=state(this);
      return value.toDate ||= new Date(value.data.validToMilliseconds);
    }
    get raw() { const value=state(this); return value.raw ||= Buffer.from(value.bytes); }
    get ca() { return state(this).data.ca; }
    // A standalone parsed certificate carries no issuer chain, as in Node.
    get issuerCertificate() { state(this); return undefined; }
    toString() { return state(this).data.pem; }
    toJSON() { return this.toString(); }
    checkHost(name,options) { return check(this,'host',name,options); }
    checkEmail(name,options) { return check(this,'email',name,options); }
    checkIP(name,options) { return check(this,'ip',name,options); }
    checkIssued(other) {
      const issuer=states.get(other);
      if(!issuer) throw invalid('otherCert');
      return call('issued',state(this).bytes,issuer.bytes);
    }
    get subjectAltName() { state(this); return unsupported('subjectAltName'); }
    get infoAccess() { state(this); return unsupported('infoAccess'); }
    get keyUsage() { state(this); return unsupported('keyUsage'); }
    get publicKey() { state(this); return unsupported('publicKey'); }
    checkPrivateKey() { state(this); return unsupported('checkPrivateKey'); }
    verify() { state(this); return unsupported('verify'); }
    toLegacyObject() { state(this); return unsupported('toLegacyObject'); }
  }
  return X509Certificate;
}

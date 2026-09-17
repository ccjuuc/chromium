  // These modules are importable for dependency discovery. TLS operations require
  // a TLS transport; a plain net socket must never stand in for encryption.
  const tlsModule = (() => {
    const unavailable = operation => {
      const error = new Error(`TLS ${operation} is not supported by this runtime`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    };
    class TLSSocket extends EventEmitter {
      constructor() { super(); unavailable('TLSSocket'); }
    }
    class Server extends EventEmitter {
      constructor() { super(); unavailable('Server'); }
    }
    class SecureContext {
      constructor() { unavailable('SecureContext'); }
    }
    return {
      TLSSocket, Server, SecureContext,
      connect: () => unavailable('connect'),
      createServer: () => unavailable('createServer'),
      createSecureContext: () => unavailable('createSecureContext'),
      checkServerIdentity: () => unavailable('checkServerIdentity'),
      getCiphers: () => unavailable('getCiphers'),
    };
  })();

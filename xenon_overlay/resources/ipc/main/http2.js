  // HTTP/2 protocol constants are data, not evidence of an available session.
  // Keep this module distinct from HTTP/1 and fail when transport is requested.
  const http2Module = (() => {
    const unavailable = operation => {
      const error = new Error(`http2.${operation} is not supported by this runtime`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    };
    class Http2ServerRequest extends EventEmitter {
      constructor() { super(); unavailable('Http2ServerRequest'); }
    }
    class Http2ServerResponse extends EventEmitter {
      constructor() { super(); unavailable('Http2ServerResponse'); }
    }
    const constants = {
      HTTP2_HEADER_SCHEME: ':scheme',
      HTTP2_HEADER_METHOD: ':method',
      HTTP2_HEADER_PATH: ':path',
      HTTP2_HEADER_STATUS: ':status',
      HTTP2_HEADER_AUTHORITY: ':authority',
    };
    return {
      constants,
      sensitiveHeaders: Symbol('sensitiveHeaders'),
      Http2ServerRequest, Http2ServerResponse,
      connect: () => unavailable('connect'),
      createServer: () => unavailable('createServer'),
      createSecureServer: () => unavailable('createSecureServer'),
      performServerHandshake: () => unavailable('performServerHandshake'),
      getPackedSettings: () => unavailable('getPackedSettings'),
      getUnpackedSettings: () => unavailable('getUnpackedSettings'),
      getDefaultSettings: () => Object.assign(Object.create(null), {
        headerTableSize: 4096, enablePush: true, initialWindowSize: 65535,
        maxFrameSize: 16384, maxConcurrentStreams: 4294967295,
        maxHeaderSize: 65535, maxHeaderListSize: 65535,
        enableConnectProtocol: false,
      }),
    };
  })();

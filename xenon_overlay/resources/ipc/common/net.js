// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createXenonNetModule({platform, idPrefix, send, setDispatch}) {
  let dispatchXenonNet;
  // Node `net` OS pipes (Windows named pipes and Unix domain sockets).
  // Local pairing is available only after a successful native bind; other
  // endpoints use the reserved IPC channels.
  // TCP endpoints always use the native OS transport.
  function normalizeNetPath(value) {
    let path = String(value || '');
    if (platform === 'win32') {
      path = path.replace(/\//g, '\\').toLowerCase();
    }
    return path;
  }

  function netError(code, message, ErrorType = Error) {
    return Object.assign(new ErrorType(message), {code});
  }

  function unsupportedNetOption(operation, name) {
    throw netError('ERR_NOT_SUPPORTED', `net.${operation} does not support the "${name}" option`);
  }

  function checkNetOptions(options, operation) {
    if (!options || typeof options !== 'object') return;
    // Keep Node's default-valued options and application metadata harmless,
    // but never silently discard a requested transport/stream configuration.
    const disabled = operation === 'listen' ?
      ['ipv6Only', 'reusePort', 'exclusive'] :
      ['keepAlive', ...(operation === 'createServer' ? [] : ['pauseOnConnect'])];
    for (const name of disabled) {
      if (options[name] != null && options[name] !== false) unsupportedNetOption(operation, name);
    }
    const absent = operation === 'listen' ? ['signal'] :
      ['signal', 'onread'];
    for (const name of absent) {
      if (options[name] != null) unsupportedNetOption(operation, name);
    }
    if (operation !== 'listen') {
      for (const name of ['timeout', 'keepAliveInitialDelay']) {
        if (options[name] != null && options[name] !== 0) unsupportedNetOption(operation, name);
      }
    }
    if (operation === 'connect') {
      for (const name of ['localPort', 'family', 'hints']) {
        if (options[name] != null && options[name] !== 0) unsupportedNetOption(operation, name);
      }
      if (options.localAddress != null && options.localAddress !== '') unsupportedNetOption(operation, 'localAddress');
      for (const name of ['lookup', 'autoSelectFamilyAttemptTimeout']) {
        if (options[name] != null) unsupportedNetOption(operation, name);
      }
      if (options.autoSelectFamily != null && options.autoSelectFamily !== false) {
        unsupportedNetOption(operation, 'autoSelectFamily');
      }
    }
  }

  function netEndpoint(args, operation) {
    const first = args[0];
    if (operation === 'connect' && first === null) {
      throw netError('ERR_INVALID_ARG_TYPE', 'The first argument must be options, a port or a path', TypeError);
    }
    const options = first && typeof first === 'object' ? first :
        typeof first === 'string' && !(Number(first) >= 0) ? {path: first} :
        {port: operation === 'listen' && typeof first === 'function' ? 0 : first,
          host: typeof args[1] === 'string' ? args[1] : undefined};
    checkNetOptions(options, operation);
    if (operation === 'listen') {
      // Node accepts backlog in options or after either port/path or host.
      // The native bridge currently chooses its own backlog.
      const backlog = options.backlog || Number(args[1]) || Number(args[2]);
      if (backlog) unsupportedNetOption(operation, 'backlog');
    }
    if (options.path !== undefined && !(operation === 'listen' && 'port' in options)) {
      if (typeof options.path !== 'string') {
        throw netError('ERR_INVALID_ARG_TYPE', 'The "path" argument must be a string', TypeError);
      }
      if (options.path.includes('\0')) {
        throw netError('ERR_INVALID_ARG_VALUE', 'The "path" argument must not contain null bytes', TypeError);
      }
      if (!options.path && operation === 'listen') {
        throw netError('ERR_INVALID_ARG_VALUE', 'The "path" argument must not be empty', TypeError);
      }
      const endpoint = {path: options.path};
      if (operation === 'listen') {
        // Node applies these permissions only to pipe listeners and only for
        // the boolean true. TCP listeners ignore the pipe-specific options.
        for (const name of ['readableAll', 'writableAll']) {
          if (options[name] !== undefined) endpoint[name] = options[name] === true;
        }
      }
      return endpoint;
    }
    if (options.fd !== undefined || options.handle !== undefined) return {};
    let port = options.port;
    if (port === undefined || port === null) {
      if (operation === 'listen' && (first == null || typeof first === 'function' || 'port' in options)) {
        port = 0;
      } else {
        throw netError(operation === 'connect' ? 'ERR_MISSING_ARGS' : 'ERR_INVALID_ARG_VALUE',
            'A port or path is required', TypeError);
      }
    }
    if ((typeof port !== 'number' && typeof port !== 'string')) {
      throw netError('ERR_INVALID_ARG_TYPE', 'The "port" argument must be a number or string', TypeError);
    }
    if ((typeof port === 'string' && !port.trim()) ||
        !Number.isInteger(Number(port)) || Number(port) < 0 || Number(port) > 65535) {
      throw netError('ERR_SOCKET_BAD_PORT', 'Port must be >= 0 and < 65536', RangeError);
    }
    if (options.host !== undefined && typeof options.host !== 'string') {
      throw netError('ERR_INVALID_ARG_TYPE', 'The "host" argument must be a string', TypeError);
    }
    return {port: Number(port), host: options.host};
  }

  function unsupportedNetEndpoint(operation, endpoint) {
    const error = netError('ERR_NOT_SUPPORTED',
        `net.${operation} does not support this endpoint type`);
    error.syscall = operation;
    if (endpoint.path !== undefined) error.path = endpoint.path;
    if (endpoint.port !== undefined) error.port = endpoint.port;
    if (endpoint.host !== undefined) error.address = endpoint.host;
    return error;
  }

  function netIsIPv4(input) {
    return /^(?:(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])\.){3}(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])$/.test(input);
  }

  function netIsIPv6(input) {
    const match = /^([0-9a-f:.]+)(?:%[0-9a-z.:-]+)?$/i.exec(input);
    if (!match) return false;
    let address = match[1];
    if (address.includes('.')) {
      const colon = address.lastIndexOf(':');
      if (colon < 0 || !netIsIPv4(address.slice(colon + 1))) return false;
      address = address.slice(0, colon + 1) + '0:0';
    }
    const halves = address.split('::');
    if (halves.length > 2) return false;
    const groups = halves.flatMap(half => half ? half.split(':') : []);
    if (!groups.every(group => /^[0-9a-f]{1,4}$/i.test(group))) return false;
    return halves.length === 2 ? groups.length < 8 : groups.length === 8;
  }

  function bytesToNetWire(data) {
    if (typeof data === 'string') {
      return {t: 's', d: data};
    }
    const u8 = data instanceof Uint8Array ? data : Buffer.from(String(data));
    return {t: 'b64', d: Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength).toString('base64')};
  }

  function netWireToBytes(wire) {
    if (!wire || wire.t === 's') {
      return Buffer.from(String(wire && wire.d || ''), 'utf8');
    }
    if (wire.t === 'b64') {
      return Buffer.from(String(wire.d || ''), 'base64');
    }
    const raw = String(wire.d || '');
    const u8 = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) {
      u8[i] = raw.charCodeAt(i) & 0xff;
    }
    return Buffer.from(u8);
  }

  const netServers = new Map();
  const nativeNetServers = new Map();
  const netSockets = new Map();
  let nextNetSocketId = 1;
  let nextNetServerId = 1;

  function isNetPipePath(path) {
    return platform === 'win32' ? /^\\\\\.\\pipe\\.+/i.test(String(path || '')) :
        typeof path === 'string' && path.length > 0;
  }

  function sendXenonNet(channel, payload) {
    try {
      send(channel, payload);
    } catch (error) {
      // Submission failures must release pending resources just like native
      // bind/connect errors; otherwise callers wait forever for a reply.
      queueMicrotask(() => dispatchXenonNet('__xenon:net:error', {
        serverId: payload.serverId,
        toId: payload.fromId,
        code: error.code || 'ERR_NOT_SUPPORTED',
        path: payload.path,
      }));
      return false;
    }
    return true;
  }

  // Socket and server callbacks belong to their connect/listen resource,
  // even when delivered by a later IPC message or the other local endpoint.
  function callNetCallback(resource, callback, args = []) {
    return runWithAsyncContext(resource._asyncContext, callback, resource, args);
  }
  function emitNetEvent(resource, event, ...args) {
    return callNetCallback(resource, resource.emit, [event, ...args]);
  }

  function allocNetSocket(socket) {
    socket._id = idPrefix + (nextNetSocketId++);
    netSockets.set(socket._id, socket);
    return socket._id;
  }

  function flushPendingNetWrites(socket) {
    if (socket.destroyed || !socket._connectReady) return;
    if (socket._activeWrite && !socket._activeWrite.sent) socket._submitWrite();
    if (socket._finalCallback && !socket._shutdownSent) socket._submitFinal();
  }

  function pauseNativeRead(socket, paused) {
    if (socket._nativeReadPaused === paused) return;
    socket._nativeReadPaused = paused;
    if (socket._peerId && !socket.destroyed) {
      sendXenonNet(paused ? '__xenon:net:pause' : '__xenon:net:resume',
        {toId: socket._peerId, fromId: socket._id});
    }
  }

  function refreshNetTimeout(socket) {
    if (socket._timeoutTimer !== null) clearTimeout(socket._timeoutTimer);
    socket._timeoutTimer = null;
    if (!socket.timeout || socket._closed) return;
    socket._timeoutTimer = setTimeout(() => {
      socket._timeoutTimer = null;
      if (!socket._closed) emitNetEvent(socket, 'timeout');
    }, socket.timeout);
    // Node's socket inactivity timer does not keep the process alive.
    socket._timeoutTimer?.unref?.();
  }

  function deliverNetBytes(socket, data) {
    if (socket.destroyed || socket._readableState.ended) return false;
    const bytes = Buffer.from(data);
    socket.bytesRead += bytes.length;
    if (bytes.length) refreshNetTimeout(socket);
    const accepted = callNetCallback(socket, socket.push, [bytes]);
    if (!accepted) pauseNativeRead(socket, true);
    return accepted;
  }

  function receiveNetEnd(socket) {
    if (!socket.destroyed && !socket._readableState.ended) {
      callNetCallback(socket, socket.push, [null]);
    }
  }

  function receiveNetClose(socket) {
    if (socket.destroyed) return;
    socket._nativeClosed = true;
    receiveNetEnd(socket);
    // A close can arrive after the final data IPC while the consumer is
    // paused. Preserve the Duplex read buffer until it is consumed.
    if (socket.readableLength && !socket.readableEnded) {
      socket.once('end', () => socket.destroy());
    } else callNetCallback(socket, socket.destroy);
  }

  function completeNetConnect(socket) {
    socket.connecting = false;
    socket._connectReady = true;
    refreshNetTimeout(socket);
    if (socket._noDelay !== undefined) socket.setNoDelay(socket._noDelay);
    emitNetEvent(socket, 'connect');
    socket._connectCb = null;
    if (socket.destroyed) return;
    if (!socket.destroyed) {
      emitNetEvent(socket, 'ready');
      flushPendingNetWrites(socket);
      if (!socket.destroyed && !socket.isPaused()) callNetCallback(socket, socket.read, [0]);
    }
  }

  function completeNetServerClose(server) {
    if (!server._nativeClosed || server._localConnections !== 0) return;
    server._nativeClosed = false;
    server._closeCb = null;
    emitNetEvent(server, 'close');
  }

  function applySocketAddress(socket, message) {
    for (const prefix of ['local', 'remote']) {
      const address = message[prefix];
      if (address) {
        socket[prefix + 'Address'] = address.address;
        socket[prefix + 'Port'] = address.port;
        socket[prefix + 'Family'] = address.family;
      }
    }
  }

  const netModule = {
    Socket: class extends Duplex {
      constructor(options = {}) {
        checkNetOptions(options, 'Socket');
        super({allowHalfOpen: Boolean(options.allowHalfOpen), autoDestroy: true,
          emitClose: true, decodeStrings: true,
          highWaterMark: options.highWaterMark,
          readableHighWaterMark: options.readableHighWaterMark,
          writableHighWaterMark: options.writableHighWaterMark});
        this._asyncContext = currentAsyncContext;
        this._connected = false;
        this._connectReady = false;
        this._closed = false;
        this._peer = null;
        this._peerId = null;
        this._id = null;
        this._connectCb = null;
        this._activeWrite = null;
        this._nextWriteId = 1;
        this._finalCallback = null;
        this._shutdownSent = false;
        this._nativeReadPaused = false;
        this._nativeClosed = false;
        this._noDelay = options.noDelay === undefined ? undefined : Boolean(options.noDelay);
        this.timeout = 0;
        this._timeoutTimer = null;
        this.connecting = false;
        this.remoteAddress = '';
        this.localAddress = '';
        this._tcp = false;
        this.bytesRead = 0;
        this._bytesWritten = 0;
      }
      emit(event, ...args) {
        if (event === 'close') args = [Boolean(this._hadError)];
        return super.emit(event, ...args);
      }
      destroy(error) { return callNetCallback(this, super.destroy, [error]); }
      get bytesWritten() { return this._bytesWritten + this.writableLength; }
      get bufferSize() { return this.destroyed ? undefined : this.writableLength; }
      get pending() { return !this._connected || this.connecting; }
      get readyState() {
        return this.connecting ? 'opening' : this.readable && this.writable ? 'open' :
          this.readable ? 'readOnly' : this.writable ? 'writeOnly' : 'closed';
      }
      connect(...args) {
        const endpoint = netEndpoint(args, 'connect');
        if (this.connecting || this._connected) throw netError('EISCONN', 'Socket is already connected');
        if (this.destroyed) {
          // Node revives the existing stream state: listeners, decoder,
          // unread data and explicit pause state survive a reconnect.
          this._undestroy();
          this.bytesRead = this._bytesWritten = 0;
        }
        this._asyncContext = currentAsyncContext;
        this._connectCb = typeof args[args.length - 1] === 'function' ? args[args.length - 1] : null;
        if (this._connectCb) this.once('connect', this._connectCb);
        this._closed = false;
        this._nativeClosed = false;
        this._shutdownSent = false;
        this._hadError = false;
        this._connectReady = false;
        this.connecting = true;
        this._tcp = endpoint.port !== undefined;
        if (args[0] && typeof args[0] === 'object' && args[0].noDelay !== undefined) {
          this._noDelay = Boolean(args[0].noDelay);
        }
        const path = endpoint.path === undefined ? undefined : normalizeNetPath(endpoint.path);
        if (!this._tcp && !isNetPipePath(path)) {
          queueMicrotask(() => { if (!this.destroyed) this.destroy(unsupportedNetEndpoint('connect', endpoint)); });
          return this;
        }
        allocNetSocket(this);
        refreshNetTimeout(this);
        const server = !this._tcp && netServers.get(path);
        if (server) {
          const incoming = new netModule.Socket({allowHalfOpen: server._allowHalfOpen,
            highWaterMark: server._highWaterMark});
          incoming._asyncContext = server._asyncContext;
          incoming._connected = incoming._connectReady = true;
          incoming._peer = this;
          ++server._localConnections;
          incoming.once('close', () => {
            --server._localConnections;
            completeNetServerClose(server);
          });
          allocNetSocket(incoming);
          if (server._pauseOnConnect) incoming.pause();
          this._peer = incoming;
          this._connected = true;
          queueMicrotask(() => {
            if (this.destroyed || incoming.destroyed) return;
            emitNetEvent(server, 'connection', incoming);
            if (this.destroyed || incoming.destroyed) return;
            completeNetConnect(this);
          });
          return this;
        }
        sendXenonNet('__xenon:net:connect', {...endpoint, path, fromId: this._id});
        return this;
      }
      _read() {
        pauseNativeRead(this, false);
        // Local pairs apply pressure through the same Duplex push result.
        // Completing this write lets Writable send the next buffered chunk.
        const writer = this._peer;
        if (writer?._activeWrite?.blocked) writer._completeWrite();
      }
      _write(chunk, encoding, callback) {
        this._activeWrite = {chunk: Buffer.from(chunk), callback, sent: false,
          id: this._nextWriteId++, blocked: false};
        if (!this._connected && !this.connecting) {
          this._completeWrite(netError('ERR_SOCKET_CLOSED', 'Socket is closed'));
        } else if (this._connectReady) this._submitWrite();
      }
      _submitWrite() {
        const write = this._activeWrite;
        if (!write || write.sent || this.destroyed) return;
        write.sent = true;
        refreshNetTimeout(this);
        if (this._peer) {
          const peer = this._peer;
          if (peer.destroyed) return this._completeWrite(netError('EPIPE', 'write EPIPE'));
          const accepted = deliverNetBytes(peer, write.chunk);
          // Listener code may synchronously close either endpoint.
          if (this._activeWrite !== write || this.destroyed) return;
          if (!accepted) write.blocked = true;
          else queueMicrotask(() => { if (this._activeWrite === write) this._completeWrite(); });
        } else if (this._peerId) {
          sendXenonNet('__xenon:net:data', {toId: this._peerId, fromId: this._id,
            writeId: write.id, wire: bytesToNetWire(write.chunk)});
        }
      }
      _completeWrite(error) {
        const write = this._activeWrite;
        if (!write) return;
        this._activeWrite = null;
        if (!error) this._bytesWritten += write.chunk.length;
        callNetCallback(this, write.callback, error ? [error] : []);
      }
      _final(callback) {
        this._finalCallback = callback;
        if (this._connectReady) this._submitFinal();
        else if (!this.connecting) {
          this._finalCallback = null;
          callback(netError('ERR_SOCKET_CLOSED', 'Socket is closed'));
        }
      }
      _submitFinal() {
        if (this.destroyed || this._shutdownSent || !this._finalCallback) return;
        this._shutdownSent = true;
        if (this._peer) {
          receiveNetEnd(this._peer);
          queueMicrotask(() => this._completeFinal());
        } else if (this._peerId) {
          sendXenonNet('__xenon:net:end', {toId: this._peerId, fromId: this._id});
        }
      }
      _completeFinal(error) {
        const callback = this._finalCallback;
        if (!callback) return;
        this._finalCallback = null;
        callNetCallback(this, callback, error ? [error] : []);
      }
      _destroy(error, callback) {
        this._closed = true;
        this._hadError = Boolean(error);
        const wasConnecting = this.connecting;
        this.connecting = false;
        this._connected = this._connectReady = false;
        if (this._connectCb) this.removeListener('connect', this._connectCb);
        this._connectCb = null;
        if (this._timeoutTimer !== null) clearTimeout(this._timeoutTimer);
        this._timeoutTimer = null;
        if (this._id) netSockets.delete(this._id);
        const peer = this._peer;
        this._peer = null;
        if (peer) {
          peer._peer = null;
          receiveNetClose(peer);
        }
        if (!this._nativeClosed && this._peerId) {
          sendXenonNet('__xenon:net:close', {toId: this._peerId, fromId: this._id});
        } else if (!peer && wasConnecting && this._id) {
          // The OS handle may exist before its connected IPC returns.
          sendXenonNet('__xenon:net:cancel', {fromId: this._id});
        }
        this._peerId = null;
        const pendingError = error || netError('ERR_STREAM_DESTROYED', 'Socket closed before operation completed');
        this._completeWrite(wasConnecting ?
          netError('ERR_SOCKET_CLOSED_BEFORE_CONNECTION', 'Socket closed before connection was established') : pendingError);
        this._completeFinal(pendingError);
        callback(error);
      }
      pause() {
        super.pause();
        pauseNativeRead(this, true);
        return this;
      }
      resume() {
        callNetCallback(this, super.resume);
        // Readable consumes its existing buffer before asking _read for more.
        // Do not restart native reads synchronously ahead of that consumption.
        callNetCallback(this, () => queueMicrotask(() => {
          if (!this.destroyed && !this.isPaused() && !this._readableState.ended &&
              this.readableLength < this.readableHighWaterMark) pauseNativeRead(this, false);
        }));
        return this;
      }
      setTimeout(msecs, callback) {
        if (typeof msecs !== 'number') {
          throw netError('ERR_INVALID_ARG_TYPE', 'The "msecs" argument must be a number', TypeError);
        }
        if (!Number.isFinite(msecs) || msecs < 0) {
          throw netError('ERR_OUT_OF_RANGE', 'The "msecs" argument must be a non-negative finite number', RangeError);
        }
        if (callback !== undefined && typeof callback !== 'function') {
          throw netError('ERR_INVALID_ARG_TYPE', 'The "callback" argument must be a function', TypeError);
        }
        this.timeout = msecs;
        if (callback) {
          if (msecs === 0) this.removeListener('timeout', callback);
          else this.once('timeout', callback);
        }
        refreshNetTimeout(this);
        return this;
      }
      setNoDelay(noDelay = true) {
        this._noDelay = Boolean(noDelay);
        if (this._tcp && this._peerId && !this.destroyed) {
          sendXenonNet('__xenon:net:no-delay', {toId: this._peerId,
            fromId: this._id, noDelay: this._noDelay});
        }
        return this;
      }
      setKeepAlive() {
        if (this._tcp) throw netError('ERR_NOT_SUPPORTED', 'TCP keep-alive configuration is not supported');
        return this;
      }
      address() {
        return this.localPort === undefined ? {} :
          {address: this.localAddress, port: this.localPort, family: this.localFamily};
      }
      ref() { return this; }
      unref() { return this; }
    },
    Server: class extends EventEmitter {
      constructor(options, connectionListener) {
        super();
        checkNetOptions(options, 'createServer');
        this.listening = false;
        this._listeningPending = false;
        this._closing = false;
        this._nativeClosed = false;
        this._localConnections = 0;
        this._path = null;
        this._address = null;
        this._allowHalfOpen = Boolean(options?.allowHalfOpen);
        this._highWaterMark = options?.highWaterMark;
        this._noDelay = options?.noDelay;
        this._pauseOnConnect = Boolean(options?.pauseOnConnect);
        const listener = typeof options === 'function' ? options : connectionListener;
        if (listener !== undefined) this.on('connection', listener);
      }
      listen(...args) {
        if (this.listening || this._listeningPending || this._nativeId) {
          throw netError('ERR_SERVER_ALREADY_LISTEN', 'Listen method has been called more than once without closing');
        }
        const endpoint = netEndpoint(args, 'listen');
        this._asyncContext = currentAsyncContext;
        const cb = typeof args[args.length - 1] === 'function' ?
            args[args.length - 1] : null;
        this._path = endpoint.path ? normalizeNetPath(endpoint.path) : null;
        this._address = endpoint.path || null;
        this._tcp = endpoint.port !== undefined;
        this._listeningPending = true;
        this._closing = false;
        if (this._tcp || isNetPipePath(this._path)) {
          this._nativeId = 'server-' + idPrefix + (nextNetServerId++);
          this._listenCb = cb;
          if (cb) this.once('listening', cb);
          nativeNetServers.set(this._nativeId, this);
          sendXenonNet('__xenon:net:listen', {
            ...endpoint, serverId: this._nativeId,
            ...(this._tcp ? {} : {path: this._path}),
          });
          return this;
        }
        queueMicrotask(() => {
          this._listeningPending = false;
          this._path = null;
          emitNetEvent(this, 'error', unsupportedNetEndpoint('listen', endpoint));
        });
        return this;
      }
      close(cb) {
        if (netServers.get(this._path) === this) {
          netServers.delete(this._path);
        }
        this.listening = false;
        this._listeningPending = false;
        this._closing = true;
        if (this._listenCb) this.removeListener('listening', this._listenCb);
        this._listenCb = null;
        if (this._nativeId) {
          this._closeCb = typeof cb === 'function' ? cb : null;
          if (this._closeCb) this.once('close', this._closeCb);
          sendXenonNet('__xenon:net:unlisten', {serverId: this._nativeId});
          return this;
        }
        this._path = null;
        queueMicrotask(() => {
          emitNetEvent(this, 'close');
          if (typeof cb === 'function') {
            callNetCallback(this, cb, [netError('ERR_SERVER_NOT_RUNNING', 'Server is not running')]);
          }
        });
        return this;
      }
      address() {
        return this.listening ? this._address : null;
      }
      ref() { return this; }
      unref() { return this; }
    },
    createServer: (...args) => new netModule.Server(...args),
    connect: (...args) => {
      const socket = new netModule.Socket(args[0] && typeof args[0] === 'object' ? args[0] : undefined);
      socket.connect(...args);
      return socket;
    },
    createConnection: (...args) => netModule.connect(...args),
    isIP: (input) => netIsIPv4(input) ? 4 : netIsIPv6(input) ? 6 : 0,
    isIPv4: netIsIPv4,
    isIPv6: netIsIPv6,
  };
  netModule.default = netModule;

  dispatchXenonNet = (channel, payload) => {
    const msg = payload || {};
    if (channel === '__xenon:net:listening') {
      const server = nativeNetServers.get(msg.serverId);
      if (server && server._listeningPending && !server._closing) {
        server._listeningPending = false;
        server.listening = true;
        if (msg.address !== undefined) server._address = msg.address;
        if (!server._tcp) netServers.set(server._path, server);
        server._listenCb = null;
        emitNetEvent(server, 'listening');
      }
      return true;
    }
    if (channel === '__xenon:net:connection') {
      const server = nativeNetServers.get(msg.serverId);
      if (!server || !server.listening || !msg.socketId) {
        if (msg.socketId) sendXenonNet('__xenon:net:close', {toId: msg.socketId});
        return true;
      }
      const incoming = new netModule.Socket({allowHalfOpen: server._allowHalfOpen,
        highWaterMark: server._highWaterMark, noDelay: server._noDelay});
      incoming._asyncContext = server._asyncContext;
      incoming._id = msg.socketId;
      incoming._peerId = msg.socketId;
      incoming._connected = incoming._connectReady = true;
      incoming._tcp = server._tcp;
      applySocketAddress(incoming, msg);
      netSockets.set(incoming._id, incoming);
      if (server._pauseOnConnect) incoming.pause();
      if (incoming._noDelay !== undefined) incoming.setNoDelay(incoming._noDelay);
      emitNetEvent(server, 'connection', incoming);
      return true;
    }
    if (channel === '__xenon:net:server-closed') {
      const server = nativeNetServers.get(msg.serverId);
      if (server) {
        nativeNetServers.delete(msg.serverId);
        server._nativeId = null;
        if (netServers.get(server._path) === server) netServers.delete(server._path);
        server._path = null;
        server.listening = false;
        server._listeningPending = false;
        server._listenCb = null;
        server._nativeClosed = true;
        completeNetServerClose(server);
      }
      return true;
    }
    if (channel === '__xenon:net:connect') {
      const server = netServers.get(normalizeNetPath(msg.path));
      if (!server) {
        sendXenonNet('__xenon:net:error', {
          toId: msg.fromId,
          code: 'ECONNREFUSED',
          path: msg.path,
        });
        return true;
      }
      const incoming = new netModule.Socket({allowHalfOpen: server._allowHalfOpen,
        highWaterMark: server._highWaterMark, noDelay: server._noDelay});
      incoming._asyncContext = server._asyncContext;
      incoming._connected = incoming._connectReady = true;
      incoming._peerId = msg.fromId;
      allocNetSocket(incoming);
      if (server._pauseOnConnect) incoming.pause();
      // Match net.Server ordering: install connection/data listeners before
      // the peer observes its connect event.
      emitNetEvent(server, 'connection', incoming);
      sendXenonNet('__xenon:net:connected', {
        toId: msg.fromId,
        peerId: incoming._id,
      });
      return true;
    }
    if (channel === '__xenon:net:connected') {
      const socket = netSockets.get(msg.toId);
      if (!socket) {
        if (msg.peerId) sendXenonNet('__xenon:net:close', {toId: msg.peerId});
        return true;
      }
      if (socket._connected) return true;
      applySocketAddress(socket, msg);
      socket._peerId = msg.peerId;
      socket._connected = true;
      if (socket._nativeReadPaused) sendXenonNet('__xenon:net:pause', {toId: socket._peerId, fromId: socket._id});
      completeNetConnect(socket);
      return true;
    }
    if (channel === '__xenon:net:data') {
      const socket = netSockets.get(msg.toId);
      if (socket) {
        const bytes = netWireToBytes(msg.wire);
        deliverNetBytes(socket, bytes);
      }
      return true;
    }
    if (channel === '__xenon:net:written') {
      const socket = netSockets.get(msg.toId);
      if (socket?._activeWrite?.id === msg.writeId) {
        socket._completeWrite(msg.code ? netError(msg.code, msg.code) : undefined);
      }
      return true;
    }
    if (channel === '__xenon:net:finish') {
      const socket = netSockets.get(msg.toId);
      if (socket) socket._completeFinal();
      return true;
    }
    if (channel === '__xenon:net:end') {
      const socket = netSockets.get(msg.toId);
      if (socket) receiveNetEnd(socket);
      return true;
    }
    if (channel === '__xenon:net:close') {
      const socket = netSockets.get(msg.toId);
      if (socket) {
        receiveNetClose(socket);
      }
      return true;
    }
    if (channel === '__xenon:net:error') {
      if (msg.serverId) {
        const server = nativeNetServers.get(msg.serverId);
        if (server) {
          const err = new Error(msg.code || 'net error');
          err.code = msg.code;
          err.syscall = 'listen';
          err.path = server._path;
          nativeNetServers.delete(msg.serverId);
          if (netServers.get(server._path) === server) netServers.delete(server._path);
          server._nativeId = null;
          if (server._listenCb) server.removeListener('listening', server._listenCb);
          server._listenCb = null;
          server._path = null;
          server.listening = false;
          server._listeningPending = false;
          emitNetEvent(server, 'error', err);
        }
        return true;
      }
      const socket = netSockets.get(msg.toId);
      if (socket) {
        const err = new Error(msg.code || 'net error');
        err.code = msg.code;
        if (msg.path !== undefined) err.path = msg.path;
        socket.destroy(err);
      }
      return true;
    }
    return false;
  };

  setDispatch(dispatchXenonNet);
  return netModule;
}

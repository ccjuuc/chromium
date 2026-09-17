  // Node `net` Windows named pipes. Local pairing is available only after a
  // successful native bind; other endpoints use the reserved IPC channels.
  // TCP and Unix sockets have no renderer backend and must not appear bound.
  function normalizeNetPath(value) {
    let path = String(value || '');
    path = path.replace(/\//g, '\\');
    if (process.platform === 'win32') {
      path = path.toLowerCase();
    }
    return path;
  }

  function netError(code, message, ErrorType = Error) {
    return Object.assign(new ErrorType(message), {code});
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
      return {path: options.path};
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
        `net.${operation} supports only Windows named pipes in this runtime`);
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

  function isNamedPipePath(path) {
    return process.platform === 'win32' && /^\\\\\.\\pipe\\.+/i.test(String(path || ''));
  }

  function sendXenonNet(channel, payload) {
    try {
      transport.send(channel, payload);
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
    socket._id = 'r-' + (nextNetSocketId++);
    netSockets.set(socket._id, socket);
    return socket._id;
  }

  function flushPendingNetWrites(socket) {
    const pending = socket._pendingWrites.splice(0);
    for (const data of pending) {
      socket.write(data);
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

  function closeNetSocket(socket, fromPeer) {
    if (!socket || socket._closed) {
      return;
    }
    const wasConnected = socket._connected;
    socket._closed = true;
    socket.destroyed = true;
    socket._connected = false;
    socket.connecting = false;
    socket._connectCb = null;
    socket._pendingWrites.length = 0;
    if (socket._timeoutTimer !== null) clearTimeout(socket._timeoutTimer);
    socket._timeoutTimer = null;
    if (socket._id) {
      netSockets.delete(socket._id);
    }
    if (socket._peer && !fromPeer) {
      closeNetSocket(socket._peer, true);
    }
    socket._peer = null;
    if (!fromPeer && socket._peerId) {
      sendXenonNet('__xenon:net:close', {toId: socket._peerId, fromId: socket._id});
    }
    socket._peerId = null;
    if (wasConnected) emitNetEvent(socket, 'end');
    emitNetEvent(socket, 'close', !!socket._hadError);
  }

  function deliverNetBytes(socket, data) {
    if (socket._closed) return;
    const buf = Buffer.isBuffer(data) || data instanceof Uint8Array ?
        Buffer.from(data) : Buffer.from(String(data));
    if (buf.length) refreshNetTimeout(socket);
    emitNetEvent(socket, 'data', buf);
  }

  const netModule = {
    Socket: class extends EventEmitter {
      constructor() {
        super();
        this._connected = false;
        this._closed = false;
        this._peer = null;
        this._peerId = null;
        this._id = null;
        this._connectCb = null;
        this._pendingWrites = [];
        this.timeout = 0;
        this._timeoutTimer = null;
        this.connecting = false;
        this.destroyed = false;
        this.remoteAddress = '';
        this.localAddress = '';
      }
      connect(...args) {
        const endpoint = netEndpoint(args, 'connect');
        this._asyncContext = currentAsyncContext;
        const cb = typeof args[args.length - 1] === 'function' ?
            args[args.length - 1] : null;
        this._connectCb = cb;
        this._closed = false;
        this.destroyed = false;
        this._hadError = false;
        this.connecting = true;
        const path = endpoint.path === undefined ? undefined : normalizeNetPath(endpoint.path);
        if (!isNamedPipePath(path)) {
          queueMicrotask(() => {
            if (this._closed) return;
            this._hadError = true;
            try {
              emitNetEvent(this, 'error', unsupportedNetEndpoint('connect', endpoint));
            } finally {
              closeNetSocket(this, true);
            }
          });
          return this;
        }
        allocNetSocket(this);
        refreshNetTimeout(this);
        const server = netServers.get(path);
        if (server) {
          const incoming = new netModule.Socket();
          incoming._asyncContext = server._asyncContext;
          incoming._connected = true;
          incoming._peer = this;
          allocNetSocket(incoming);
          this._peer = incoming;
          this._connected = true;
          this.connecting = false;
          queueMicrotask(() => {
            if (this._closed || incoming._closed) return;
            emitNetEvent(server, 'connection', incoming);
            // A server or client connection listener can synchronously destroy
            // either endpoint. Do not continue the queued handshake afterward.
            if (this._closed || incoming._closed) return;
            emitNetEvent(this, 'connect');
            if (this._closed || incoming._closed) return;
            const callback = this._connectCb;
            this._connectCb = null;
            if (callback) callNetCallback(this, callback);
            if (!this._closed) flushPendingNetWrites(this);
          });
          return this;
        }
        sendXenonNet('__xenon:net:connect', {path, fromId: this._id});
        return this;
      }
      write(data) {
        if (this._closed) return false;
        if (this._peer) {
          refreshNetTimeout(this);
          deliverNetBytes(this._peer, data);
          return true;
        }
        if (this._peerId) {
          refreshNetTimeout(this);
          return sendXenonNet('__xenon:net:data', {
            toId: this._peerId,
            fromId: this._id,
            wire: bytesToNetWire(data),
          });
        }
        if (this.connecting && !this._closed) {
          this._pendingWrites.push(
              Buffer.isBuffer(data) || data instanceof Uint8Array ?
                  Buffer.from(data) : data);
          return true;
        }
        return false;
      }
      end(data) {
        if (data !== undefined) {
          this.write(data);
        }
        closeNetSocket(this, false);
        return this;
      }
      destroy() {
        this.destroyed = true;
        closeNetSocket(this, false);
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
      // These TCP options are chainable no-ops for pipe handles in Node.
      // TCP connect itself reports ERR_NOT_SUPPORTED above.
      setNoDelay() { return this; }
      setKeepAlive() { return this; }
      ref() { return this; }
      unref() { return this; }
    },
    Server: class extends EventEmitter {
      constructor(options, connectionListener) {
        super();
        this.listening = false;
        this._listeningPending = false;
        this._closing = false;
        this._path = null;
        this._address = null;
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
        this._listeningPending = true;
        this._closing = false;
        if (isNamedPipePath(this._path)) {
          this._nativeId = 'server-r-' + (nextNetServerId++);
          this._listenCb = cb;
          nativeNetServers.set(this._nativeId, this);
          sendXenonNet('__xenon:net:listen', {
            serverId: this._nativeId,
            path: this._path,
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
        this._listenCb = null;
        if (this._nativeId) {
          this._closeCb = typeof cb === 'function' ? cb : null;
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
      const socket = new netModule.Socket();
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
        netServers.set(server._path, server);
        const callback = server._listenCb;
        server._listenCb = null;
        emitNetEvent(server, 'listening');
        if (callback && server.listening) callNetCallback(server, callback);
      }
      return true;
    }
    if (channel === '__xenon:net:connection') {
      const server = nativeNetServers.get(msg.serverId);
      if (!server || !server.listening || !msg.socketId) {
        if (msg.socketId) sendXenonNet('__xenon:net:close', {toId: msg.socketId});
        return true;
      }
      const incoming = new netModule.Socket();
      incoming._asyncContext = server._asyncContext;
      incoming._id = msg.socketId;
      incoming._peerId = msg.socketId;
      incoming._connected = true;
      netSockets.set(incoming._id, incoming);
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
        if (server._closeCb) {
          callNetCallback(server, server._closeCb);
          server._closeCb = null;
        }
        emitNetEvent(server, 'close');
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
      const incoming = new netModule.Socket();
      incoming._asyncContext = server._asyncContext;
      incoming._connected = true;
      incoming._peerId = msg.fromId;
      allocNetSocket(incoming);
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
        return true;
      }
      socket._peerId = msg.peerId;
      socket._connected = true;
      socket.connecting = false;
      refreshNetTimeout(socket);
      emitNetEvent(socket, 'connect');
      if (socket._closed) return true;
      if (socket._connectCb) {
        callNetCallback(socket, socket._connectCb);
        socket._connectCb = null;
      }
      flushPendingNetWrites(socket);
      return true;
    }
    if (channel === '__xenon:net:data') {
      const socket = netSockets.get(msg.toId);
      if (socket) {
        deliverNetBytes(socket, netWireToBytes(msg.wire));
      }
      return true;
    }
    if (channel === '__xenon:net:close') {
      const socket = netSockets.get(msg.toId);
      if (socket) {
        closeNetSocket(socket, true);
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
        socket.connecting = false;
        socket._pendingWrites.length = 0;
        socket._hadError = true;
        try {
          emitNetEvent(socket, 'error', err);
        } finally {
          closeNetSocket(socket, true);
        }
      }
      return true;
    }
    return false;
  };

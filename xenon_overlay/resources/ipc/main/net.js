  const netModule = (() => {
    function normalizeNetPath(value) {
      let path = String(value || '');
      path = path.replace(/\//g, '\\');
      if (typeof __xenonPlatform === 'string' && __xenonPlatform === 'win32') {
        path = path.toLowerCase();
      }
      return path;
    }
    function netPathFromListenOrConnect(args) {
      const copy = args.slice();
      if (typeof copy[copy.length - 1] === 'function') {
        copy.pop();
      }
      const first = copy[0];
      if (first && typeof first === 'object') {
        return first.path || first.handle || '';
      }
      if (typeof first === 'string') {
        return first;
      }
      if (typeof first === 'number') {
        return `tcp:${copy[1] || '127.0.0.1'}:${first}`;
      }
      return '';
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
      return /^\\\\\.\\pipe\\/i.test(String(path || ''));
    }

    function sendXenonNet(channel, payload, endpointId) {
      if (typeof __xenonNetSend === 'function') {
        __xenonNetSend(channel, payload);
        return;
      }
      const error = new Error('The native net transport is unavailable');
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
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
      socket._id = 'm-' + (nextNetSocketId++);
      netSockets.set(socket._id, socket);
      return socket._id;
    }
    function flushPendingNetWrites(socket) {
      const pending = socket._pendingWrites.splice(0);
      for (const data of pending) {
        socket.write(data);
      }
    }
    function closeNetSocket(socket, fromPeer) {
      if (!socket || socket._closed) {
        return;
      }
      socket._closed = true;
      socket._connected = false;
      socket.connecting = false;
      socket._connectCb = null;
      socket._pendingWrites.length = 0;
      if (socket._id) {
        netSockets.delete(socket._id);
      }
      if (socket._peer && !fromPeer) {
        closeNetSocket(socket._peer, true);
      }
      socket._peer = null;
      if (!fromPeer && socket._peerId) {
        sendXenonNet(
            '__xenon:net:close', {toId: socket._peerId, fromId: socket._id},
            socket._endpointId);
      }
      emitNetEvent(socket, 'end');
      emitNetEvent(socket, 'close');
    }
    function deliverNetBytes(socket, data) {
      const buf = Buffer.isBuffer(data) || data instanceof Uint8Array ?
          Buffer.from(data) : Buffer.from(String(data));
      emitNetEvent(socket, 'data', buf);
    }

    const module = {
      Socket: class extends EventEmitter {
        constructor() {
          super();
          this._connected = false;
          this._closed = false;
          this._peer = null;
          this._peerId = null;
          this._id = null;
          this._connectCb = null;
          this._endpointId = '*';
          this._pendingWrites = [];
          this.connecting = false;
          this.destroyed = false;
        }
        connect(...args) {
          this._asyncContext = currentAsyncContext;
          const cb = typeof args[args.length - 1] === 'function' ?
              args[args.length - 1] : null;
          this._connectCb = cb;
          this.connecting = true;
          const path = normalizeNetPath(netPathFromListenOrConnect(args));
          allocNetSocket(this);
          const server = netServers.get(path);
          if (server && !isNamedPipePath(path)) {
            const incoming = new module.Socket();
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
              if (this._closed || incoming._closed) return;
              emitNetEvent(this, 'connect');
              if (!this._closed && this._connectCb) {
                const callback = this._connectCb;
                this._connectCb = null;
                callNetCallback(this, callback);
              }
            });
            return this;
          }
          console.info('[xenon-net] connect', path);
          sendXenonNet('__xenon:net:connect', {path, fromId: this._id});
          return this;
        }
        write(data) {
          if (this._closed) return false;
          if (this._peer) {
            deliverNetBytes(this._peer, data);
            return true;
          }
          if (this._peerId) {
            sendXenonNet('__xenon:net:data', {
              toId: this._peerId,
              fromId: this._id,
              wire: bytesToNetWire(data),
            }, this._endpointId);
            return true;
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
        }
        setTimeout() {}
        setNoDelay() {}
        setKeepAlive() {}
        ref() { return this; }
        unref() { return this; }
      },
      Server: class extends EventEmitter {
        listen(...args) {
          this._asyncContext = currentAsyncContext;
          const cb = typeof args[args.length - 1] === 'function' ?
              args[args.length - 1] : null;
          this._path = normalizeNetPath(netPathFromListenOrConnect(args));
          netServers.set(this._path, this);
          if (isNamedPipePath(this._path)) {
            this._closing = false;
            this._nativeId = 'server-m-' + (nextNetServerId++);
            this._listenCb = cb;
            nativeNetServers.set(this._nativeId, this);
            sendXenonNet('__xenon:net:listen', {
              serverId: this._nativeId,
              path: this._path,
            });
            return this;
          }
          queueMicrotask(() => {
            emitNetEvent(this, 'listening');
            if (cb) {
              callNetCallback(this, cb);
            }
          });
          return this;
        }
        close(cb) {
          if (this._path) {
            netServers.delete(this._path);
          }
          if (this._nativeId) {
            this._closing = true;
            this._closeCb = typeof cb === 'function' ? cb : null;
            this._listenCb = null;
            sendXenonNet('__xenon:net:unlisten', {serverId: this._nativeId});
            return this;
          }
          if (typeof cb === 'function') {
            callNetCallback(this, cb);
          }
          emitNetEvent(this, 'close');
          return this;
        }
        address() {
          return this._path || {port: 0, address: '127.0.0.1', family: 'IPv4'};
        }
        ref() { return this; }
        unref() { return this; }
      },
      createServer: (...args) => {
        const server = new module.Server();
        if (typeof args[0] === 'function') {
          server.on('connection', args[0]);
        }
        return server;
      },
      connect: (...args) => {
        const socket = new module.Socket();
        socket.connect(...args);
        return socket;
      },
      createConnection: (...args) => module.connect(...args),
      isIP: () => 0,
      isIPv4: () => false,
      isIPv6: () => false,
    };

    dispatchXenonNet = (channel, payload, sender) => {
      const msg = payload || {};
      const endpointId = sender && sender.endpointId ? sender.endpointId : '*';
      if (channel === '__xenon:net:listening') {
        const server = nativeNetServers.get(msg.serverId);
        if (server && !server._closing) {
          emitNetEvent(server, 'listening');
          if (server._listenCb) {
            const callback = server._listenCb;
            server._listenCb = null;
            callNetCallback(server, callback);
          }
        }
        return true;
      }
      if (channel === '__xenon:net:connection') {
        const server = nativeNetServers.get(msg.serverId);
        if (!msg.socketId) return true;
        if (!server || server._closing) {
          sendXenonNet('__xenon:net:close', {toId: msg.socketId});
          return true;
        }
        const incoming = new module.Socket();
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
          const callback = server._closeCb;
          server._closeCb = null;
          if (callback) callNetCallback(server, callback);
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
          }, endpointId);
          return true;
        }
        const incoming = new module.Socket();
        incoming._asyncContext = server._asyncContext;
        incoming._connected = true;
        incoming._peerId = msg.fromId;
        incoming._endpointId = endpointId;
        allocNetSocket(incoming);
        // A real net.Server exposes the accepted socket before the client can
        // observe connect. node-net-ipc installs its data parser here.
        emitNetEvent(server, 'connection', incoming);
        sendXenonNet('__xenon:net:connected', {
          toId: msg.fromId,
          peerId: incoming._id,
        }, endpointId);
        return true;
      }
      if (channel === '__xenon:net:connected') {
        const socket = netSockets.get(msg.toId);
        if (!socket) {
          if (msg.peerId) {
            sendXenonNet('__xenon:net:close', {toId: msg.peerId});
          }
          return true;
        }
        if (socket._connected) {
          return true;
        }
        socket._peerId = msg.peerId;
        socket._endpointId = endpointId;
        socket._connected = true;
        socket.connecting = false;
        emitNetEvent(socket, 'connect');
        if (!socket._closed && socket._connectCb) {
          const callback = socket._connectCb;
          socket._connectCb = null;
          callNetCallback(socket, callback);
        }
        // net.Socket.write() is allowed while connecting. Flush only after
        // connect listeners have run so protocol clients see normal ordering.
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
            netServers.delete(server._path);
            nativeNetServers.delete(msg.serverId);
            server._nativeId = null;
            const error = new Error(msg.code || 'net error');
            error.code = msg.code;
            emitNetEvent(server, 'error', error);
          }
          return true;
        }
        const socket = netSockets.get(msg.toId);
        if (socket && !socket._closed) {
          const err = new Error(msg.code || 'net error');
          err.code = msg.code;
          socket.connecting = false;
          socket._pendingWrites.length = 0;
          emitNetEvent(socket, 'error', err);
          closeNetSocket(socket, false);
        }
        return true;
      }
      return false;
    };

    return module;
  })();

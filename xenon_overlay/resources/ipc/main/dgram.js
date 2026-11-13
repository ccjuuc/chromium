  function createDgramModule({EventEmitter, Buffer, isIP, callNative, dispatch,
    captureContext = () => undefined, run = (_context, callback, receiver, args) => callback.apply(receiver, args)}) {
    const sockets = new Map();
    let nextRequest = 1;
    function error(code, message, Type = Error) { return Object.assign(new Type(message), {code}); }
    function nativeError(message) {
      const text = String(message);
      return error(text.split(':', 1)[0], text);
    }
    function call(request) {
      const response = callNative(request);
      if (response?.error) throw nativeError(response.error);
      if (!response || !Object.hasOwn(response, 'value'))
        throw error('ERR_INVALID_STATE', 'Invalid native UDP response');
      return response.value;
    }
    function port(value, allowZero = true) {
      if ((typeof value !== 'number' && typeof value !== 'string') ||
          (typeof value === 'string' && !value.trim()) ||
          !Number.isInteger(Number(value)) || Number(value) < (allowZero ? 0 : 1) || Number(value) > 65535)
        throw error('ERR_SOCKET_BAD_PORT', 'Port must be an integer in range', RangeError);
      return Number(value);
    }
    function text(value, name) {
      if (typeof value !== 'string' || value.includes('\0'))
        throw error('ERR_INVALID_ARG_TYPE', name + ' must be a string without NUL', TypeError);
      return value;
    }
    function bytes(value) {
      if (typeof value === 'string') return Buffer.from(value);
      if (ArrayBuffer.isView(value)) return Buffer.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      throw error('ERR_INVALID_ARG_TYPE', 'Datagram must be a string, Buffer, TypedArray or DataView', TypeError);
    }
    class Socket extends EventEmitter {
      constructor(options, listener) {
        super();
        options = typeof options === 'string' ? {type: options} : options;
        if (!options || (options.type !== 'udp4' && options.type !== 'udp6'))
          throw error('ERR_SOCKET_BAD_TYPE', 'Bad socket type specified. Valid types are: udp4, udp6', TypeError);
        if (listener !== undefined && typeof listener !== 'function')
          throw error('ERR_INVALID_ARG_TYPE', 'Listener must be a function', TypeError);
        if (options.lookup !== undefined && typeof options.lookup !== 'function')
          throw error('ERR_INVALID_ARG_TYPE', 'lookup must be a function', TypeError);
        for (const name of ['recvBufferSize', 'sendBufferSize']) {
          if (options[name] !== undefined && (!Number.isInteger(options[name]) || options[name] <= 0 || options[name] > 2147483647))
            throw error('ERR_OUT_OF_RANGE', name + ' must be a positive integer', RangeError);
        }
        if (options.signal !== undefined && (!options.signal || typeof options.signal.addEventListener !== 'function'))
          throw error('ERR_INVALID_ARG_TYPE', 'signal must be an AbortSignal', TypeError);
        this.type = options.type;
        this._options = {...options};
        this._state = 'unbound';
        this._connection = 0;
        this._context = captureContext();
        this._binding = [];
        this._lookups = new Map();
        this._sends = new Map();
        this._id = call({op: 'create', family: this.type === 'udp6' ? 6 : 4});
        sockets.set(this._id, this);
        if (listener) this.on('message', listener);
        if (options.signal) {
          this._abort = () => { if (this._state !== 'closed' && this._state !== 'closing') this.close(); };
          options.signal.addEventListener('abort', this._abort, {once: true});
          if (options.signal.aborted) queueMicrotask(this._abort);
        }
      }
      _invoke(callback, args = []) { return run(this._context, callback, this, args); }
      _emit(name, ...args) { return this._invoke(this.emit, [name, ...args]); }
      _alive() {
        if (this._state === 'closing' || this._state === 'closed')
          throw error('ERR_SOCKET_DGRAM_NOT_RUNNING', 'Not running');
      }
      _bound() { this._alive(); if (this._state !== 'bound') throw error('ERR_SOCKET_DGRAM_NOT_RUNNING', 'Not running'); }
      _call(op, options = {}) { return call({op, id: this._id, ...options}); }
      _lookup(host, callback) {
        const family = this.type === 'udp6' ? 6 : 4;
        if (isIP(host)) { queueMicrotask(() => callback(null, host)); return; }
        if (this._options.lookup) {
          let complete = false;
          this._options.lookup(host, {family}, (err, address) => {
            if (complete) return;
            complete = true;
            queueMicrotask(() => callback(err, address));
          });
          return;
        }
        const id = nextRequest++;
        this._lookups.set(id, callback);
        try { this._call('lookup', {hostname: host, lookupId: id}); }
        catch (err) { this._lookups.delete(id); queueMicrotask(() => callback(err)); }
      }
      bind(...args) {
        this._alive();
        if (this._state !== 'unbound') throw error('ERR_SOCKET_ALREADY_BOUND', 'Socket is already bound');
        const callback = typeof args.at(-1) === 'function' ? args.pop() : undefined;
        const first = args[0];
        const options = first && typeof first === 'object' ? first : {port: first, address: args[1]};
        if ('fd' in options || 'handle' in options)
          throw error('ERR_NOT_SUPPORTED', 'Binding an existing datagram handle is not supported');
        const bindPort = port(options.port ?? 0);
        const address = options.address === undefined || options.address === ''
          ? (this.type === 'udp6' ? '::' : '0.0.0.0') : text(options.address, 'address');
        if (callback) this.once('listening', callback);
        this._state = 'binding';
        this._lookup(address, (err, resolved) => {
          if (this._state !== 'binding') return;
          if (!err) {
            try {
              this._call('bind', {port: bindPort, address: resolved,
                reuseAddr: !!this._options.reuseAddr, reusePort: !!this._options.reusePort,
                ipv6Only: !!this._options.ipv6Only});
              this._state = 'bound';
              for (const [option, op] of [['recvBufferSize', 'setRecvBufferSize'], ['sendBufferSize', 'setSendBufferSize']])
                if (this._options[option] !== undefined) this._call(op, {value: this._options[option]});
            } catch (caught) { err = caught; }
          }
          const waiting = this._binding.splice(0);
          if (err) {
            // A bind failure remains retryable. A later option failure leaves
            // the successfully bound socket available for explicit closure.
            if (this._state === 'binding') this._state = 'unbound';
            if (callback) this.removeListener('listening', callback);
            for (const next of waiting) next(err);
            if (!waiting.length || this.listenerCount('error')) this._emit('error', err);
          } else {
            this._emit('listening');
            for (const next of waiting) next();
          }
        });
        return this;
      }
      _afterBind(callback) {
        if (this._state === 'bound') { callback(); return; }
        this._binding.push(callback);
        if (this._state === 'unbound') this.bind(0);
      }
      send(message, ...args) {
        this._alive();
        const callback = typeof args.at(-1) === 'function' ? args.pop() : undefined;
        let data = Array.isArray(message) ? Buffer.concat(message.map(bytes)) : bytes(message);
        let sendPort, address;
        if (this._connection === 2) {
          if (args.length === 2) {
            const [offset, length] = args;
            if (!Number.isInteger(offset) || !Number.isInteger(length) || offset < 0 || length < 0 || offset + length > data.length)
              throw error('ERR_BUFFER_OUT_OF_BOUNDS', 'Datagram offset or length is out of bounds', RangeError);
            data = data.subarray(offset, offset + length);
          } else if (args.length) throw error('ERR_SOCKET_DGRAM_IS_CONNECTED', 'Already connected');
        } else {
          if (args.length >= 3) {
            const offset = args.shift(), length = args.shift();
            if (!Number.isInteger(offset) || !Number.isInteger(length) || offset < 0 || length < 0 || offset + length > data.length)
              throw error('ERR_BUFFER_OUT_OF_BOUNDS', 'Datagram offset or length is out of bounds', RangeError);
            data = data.subarray(offset, offset + length);
          }
          sendPort = port(args[0], false);
          address = args[1] === undefined || args[1] === ''
            ? (this.type === 'udp6' ? '::1' : '127.0.0.1') : text(args[1], 'address');
          if (args.length > 2) throw error('ERR_INVALID_ARG_TYPE', 'Invalid send arguments', TypeError);
        }
        const id = nextRequest++;
        this._sends.set(id, callback);
        const finish = err => {
          if (!this._sends.has(id)) return;
          this._sends.delete(id);
          if (callback) this._invoke(callback, [err, err ? undefined : data.length]);
          else if (err) this._emit('error', err);
        };
        const send = (err, resolved) => {
          if (err) { finish(err); return; }
          try { this._alive(); this._call('send', {sendId: id, bytes: data, port: sendPort, address: resolved}); }
          catch (caught) { finish(caught); }
        };
        this._afterBind(err => {
          if (err) { queueMicrotask(() => finish(err)); return; }
          if (this._connection === 2) queueMicrotask(() => send());
          else this._lookup(address, send);
        });
      }
      connect(value, address, callback) {
        this._alive();
        if (this._connection) throw error('ERR_SOCKET_DGRAM_IS_CONNECTED', 'Already connected');
        const connectPort = port(value, false);
        if (typeof address === 'function') { callback = address; address = undefined; }
        if (callback !== undefined && typeof callback !== 'function') throw error('ERR_INVALID_ARG_TYPE', 'callback must be a function', TypeError);
        address = address === undefined || address === '' ? (this.type === 'udp6' ? '::1' : '127.0.0.1') : text(address, 'address');
        this._connection = 1;
        if (callback) this.once('connect', callback);
        this._afterBind(err => {
          const complete = (failure, resolved) => {
            if (this._state === 'closing' || this._state === 'closed') return;
            if (!failure) { try { this._call('connect', {port: connectPort, address: resolved}); } catch (caught) { failure = caught; } }
            this._connection = failure ? 0 : 2;
            if (failure) { if (callback) this.removeListener('connect', callback); this._emit('error', failure); }
            else this._emit('connect');
          };
          if (err) queueMicrotask(() => complete(err)); else this._lookup(address, complete);
        });
      }
      disconnect() {
        this._alive();
        if (this._connection !== 2) throw error('ERR_SOCKET_DGRAM_NOT_CONNECTED', 'Not connected');
        this._call('disconnect'); this._connection = 0;
      }
      address() { this._bound(); return this._call('address'); }
      remoteAddress() {
        this._alive();
        if (this._connection !== 2) throw error('ERR_SOCKET_DGRAM_NOT_CONNECTED', 'Not connected');
        return this._call('remoteAddress');
      }
      close(callback) {
        this._alive();
        if (callback !== undefined && typeof callback !== 'function') throw error('ERR_INVALID_ARG_TYPE', 'callback must be a function', TypeError);
        if (callback) this.once('close', callback);
        this._state = 'closing';
        this._call('close');
        return this;
      }
      ref() { if (this._state !== 'closed' && this._state !== 'closing') this._call('ref'); return this; }
      unref() { if (this._state !== 'closed' && this._state !== 'closing') this._call('unref'); return this; }
      setBroadcast(value) { this._bound(); this._call('setBroadcast', {value: value ? 1 : 0}); return this; }
      setTTL(value) { this._bound(); this._call('setTTL', {value: integer(value, 1, 255)}); return value; }
      setMulticastTTL(value) { this._bound(); this._call('setMulticastTTL', {value: integer(value, 0, 255)}); return value; }
      setMulticastLoopback(value) { this._bound(); this._call('setMulticastLoopback', {value: value ? 1 : 0}); return value; }
      setMulticastInterface(value) { this._bound(); this._call('setMulticastInterface', {interface: text(value, 'interface')}); return this; }
      _membership(op, group, interfaceAddress, source) {
        this._bound();
        this._call(op, {group: text(group, 'multicastAddress'),
          ...(interfaceAddress === undefined ? {} : {interface: text(interfaceAddress, 'interfaceAddress')}),
          ...(source === undefined ? {} : {source: text(source, 'sourceAddress')})});
      }
      addMembership(group, interfaceAddress) { this._membership('addMembership', group, interfaceAddress); }
      dropMembership(group, interfaceAddress) { this._membership('dropMembership', group, interfaceAddress); }
      addSourceSpecificMembership(source, group, interfaceAddress) { this._membership('addSourceSpecificMembership', group, interfaceAddress, source); }
      dropSourceSpecificMembership(source, group, interfaceAddress) { this._membership('dropSourceSpecificMembership', group, interfaceAddress, source); }
      setRecvBufferSize(value) { this._bound(); this._call('setRecvBufferSize', {value: integer(value, 1, 2147483647)}); }
      setSendBufferSize(value) { this._bound(); this._call('setSendBufferSize', {value: integer(value, 1, 2147483647)}); }
      getRecvBufferSize() { this._bound(); return this._call('getRecvBufferSize'); }
      getSendBufferSize() { this._bound(); return this._call('getSendBufferSize'); }
      getSendQueueSize() { this._alive(); return this._call('getSendQueueSize'); }
      getSendQueueCount() { this._alive(); return this._call('getSendQueueCount'); }
    }
    function integer(value, min, max) {
      if (!Number.isInteger(value) || value < min || value > max)
        throw error('ERR_OUT_OF_RANGE', 'Value is out of range', RangeError);
      return value;
    }
    if (Symbol.asyncDispose) Object.defineProperty(Socket.prototype, Symbol.asyncDispose, {value() {
      if (this._state === 'closed') return Promise.resolve();
      return new Promise(resolve => { if (this._state === 'closing') this.once('close', resolve); else this.close(resolve); });
    }});
    dispatch(event => {
      const socket = sockets.get(event.id);
      if (!socket) return;
      if (event.type === 'lookup') {
        const callback = socket._lookups.get(event.lookupId);
        socket._lookups.delete(event.lookupId);
        if (callback) callback(event.error ? nativeError(event.error) : null, event.result?.address);
      } else if (event.type === 'sent') {
        if (!socket._sends.has(event.sendId)) return;
        const callback = socket._sends.get(event.sendId);
        socket._sends.delete(event.sendId);
        const err = event.error ? nativeError(event.error) : null;
        if (callback) socket._invoke(callback, [err, err ? undefined : event.bytes]);
        else if (err) socket._emit('error', err);
      } else if (event.type === 'message') {
        if (socket._state !== 'closing') socket._emit('message', Buffer.from(event.bytes), event.rinfo);
      } else if (event.type === 'error') socket._emit('error', nativeError(event.error));
      else if (event.type === 'close') {
        sockets.delete(event.id);
        socket._state = 'closed';
        socket._options.signal?.removeEventListener('abort', socket._abort);
        socket._lookups.clear();
        socket._binding.length = 0;
        const err = error('ERR_SOCKET_DGRAM_NOT_RUNNING', 'Socket closed before sending');
        for (const callback of socket._sends.values()) if (callback) socket._invoke(callback, [err]);
        socket._sends.clear();
        socket._emit('close');
      }
    });
    return {Socket, createSocket: (options, listener) => new Socket(options, listener)};
  }

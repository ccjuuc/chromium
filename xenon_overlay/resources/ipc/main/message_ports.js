  // Native owns the real Mojo endpoints and structured cloning. JavaScript
  // owns Electron's EventEmitter objects, which are distinct from DOM ports.
  function createMessagePortMainModule({EventEmitter, host = globalThis,
      dispatchIpc, runTask = callback => callback()}) {
    const token = {};
    const state = new WeakMap();
    const references = new Map();
    const started = new Map();
    const finalizers = new FinalizationRegistry(id => {
      references.delete(id);
      host.__xenonMessagePortClose?.(id, false);
    });
    function call(name, ...args) {
      if (typeof host[name] !== 'function') {
        throw Object.assign(new Error('Native MessagePort transport is unavailable'),
            {code: 'ERR_NOT_SUPPORTED'});
      }
      return host[name](...args);
    }
    function current(port) {
      const value = state.get(port);
      if (!value) throw new TypeError('Illegal MessagePortMain receiver');
      return value;
    }
    function transfers(value) {
      if (value === undefined) return [];
      if (!Array.isArray(value)) throw new TypeError('Transfer list must be an array');
      // Copy once. Accessor side effects happen before native validation;
      // changes during structured cloning cannot change the committed list.
      const list = Array.from(value);
      for (const port of list) {
        if (!state.has(port)) throw new TypeError('Transfer list entries must be MessagePortMain objects');
      }
      return list;
    }
    function detached(list) {
      for (const port of list) {
        const value = current(port);
        value.detached = true;
        started.delete(value.id);
        references.delete(value.id);
        finalizers.unregister(port);
      }
    }
    class MessagePortMain extends EventEmitter {
      constructor(secret, id) {
        super();
        if (secret !== token) throw new TypeError('Illegal constructor');
        if (!Number.isInteger(id) || id < 1) throw new Error('Invalid native MessagePort identity');
        call('__xenonMessagePortBind', id, this);
        state.set(this, {id, started: false, closed: false, closing: false, detached: false});
        references.set(id, new WeakRef(this));
        finalizers.register(this, id, this);
      }
      postMessage(message, transfer) {
        const value = current(this);
        if (value.closed || value.closing || value.detached) return;
        const list = transfers(transfer);
        if (call('__xenonMessagePortPost', value.id, message, list)) detached(list);
      }
      start() {
        const value = current(this);
        if (value.started || value.closed || value.closing || value.detached) return;
        call('__xenonMessagePortStart', value.id);
        value.started = true;
        // A started endpoint must survive GC while native messages can arrive.
        started.set(value.id, this);
      }
      close() {
        const value = current(this);
        if (value.closed || value.closing || value.detached) return;
        call('__xenonMessagePortClose', value.id, true);
        value.closing = true;
        // Retain until the asynchronous close event has been delivered.
        started.set(value.id, this);
      }
    }
    class MessageChannelMain {
      constructor() {
        const ids = call('__xenonMessagePortCreate');
        if (!Array.isArray(ids) || ids.length !== 2) throw new Error('Invalid native channel');
        Object.defineProperties(this, {
          port1: {value: new MessagePortMain(token, ids[0]), enumerable: true},
          port2: {value: new MessagePortMain(token, ids[1]), enumerable: true},
        });
      }
    }
    host.__xenonAdoptMessagePorts = ids => ids.map(id => new MessagePortMain(token, id));
    // Host bootstrap transfers use the same facade cleanup as JS transfers,
    // including already-started wrappers retained by the keepalive map.
    host.__xenonDetachMessagePort = port => detached([port]);
    // utilityProcess.parentPort pauses native delivery while no message
    // listener exists. Ordinary MessagePortMain retains its public API.
    host.__xenonSetMessagePortPaused = (port, paused) => {
      const value = current(port);
      if (value.closed || value.closing || value.detached) return;
      if (!paused) { port.start(); return; }
      call('__xenonMessagePortPause', value.id);
      value.started = false;
      started.delete(value.id);
    };
    host.__xenonDispatchMessagePort = (id, type, data, ports) => runTask(() => {
      const port = references.get(id)?.deref();
      if (!port) return;
      const value = current(port);
      if (value.detached || value.closed) return;
      if (type === 'close') {
        value.closed = true;
        value.closing = false;
        started.delete(id);
        references.delete(id);
        finalizers.unregister(port);
        port.emit('close');
      } else if (value.started && !value.closing) {
        if (type === 'message') port.emit('message', {data, ports});
        else if (type === 'messageerror') port.emit('messageerror');
      }
    });
    host.__xenonDispatchPortMessage = (sender, channel, data, ports) =>
      runTask(() => dispatchIpc(sender, channel, data, ports));
    function postToRenderer(endpoint, channel, message, transfer) {
      if (typeof channel !== 'string') throw new TypeError('Channel must be a string');
      const list = transfers(transfer);
      if (call('__xenonPostPortMessage', endpoint, channel, message, list)) detached(list);
    }
    return {MessageChannelMain, MessagePortMain, postToRenderer};
  }

  function installMessagePortMainModule() {
    const messagePorts = createMessagePortMainModule({EventEmitter,
      runTask: callback => runWithAsyncContext(undefined, callback, undefined, []),
      dispatchIpc(sender, channel, data, ports) {
        const event = createEvent(sender);
        event.ports = ports;
        ipcMain.emit(channel, event, data);
      },
    });
    electronModule.MessageChannelMain = messagePorts.MessageChannelMain;
    electronModule.MessagePortMain = messagePorts.MessagePortMain;
    WebContents.prototype.postMessage = function(channel, message, transfer) {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const endpoint = getMainEndpoint(this);
      if (!endpoint) throw new Error('Target renderer document is unavailable');
      messagePorts.postToRenderer(endpoint.endpointId, channel, message, transfer);
    };
  }

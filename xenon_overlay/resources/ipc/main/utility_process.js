// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createUtilityProcessModule({EventEmitter, Readable, Buffer,
    MessageChannelMain, path, host = globalThis}) {
  const children = new Map(), secret = {};
  const error = (code, message, Type = Error) => Object.assign(new Type(message), {code});
  const invalid = message => error('ERR_INVALID_ARG_TYPE', message, TypeError);
  function text(value, name) {
    if (typeof value !== 'string') throw invalid(name + ' must be a string');
    if (value.includes('\0')) throw error('ERR_INVALID_ARG_VALUE', name + ' contains NUL', TypeError);
    return value;
  }
  function strings(value, name) {
    if (!Array.isArray(value)) throw invalid(name + ' must be an array of strings');
    return Array.from(value, entry => text(entry, name));
  }
  function normalize(modulePath, args, options) {
    text(modulePath, 'modulePath');
    if (!modulePath) throw error('ERR_INVALID_ARG_VALUE', 'modulePath must not be empty', TypeError);
    if (args !== undefined && !Array.isArray(args) && options === undefined) {
      options = args; args = undefined;
    }
    args = strings(args === undefined ? [] : args, 'args');
    options = options === undefined ? {} : options;
    if (!options || typeof options !== 'object' || Array.isArray(options))
      throw invalid('options must be an object');
    const allowed = new Set(['cwd', 'env', 'execArgv', 'stdio', 'serviceName',
      'allowLoadingUnsignedLibraries', 'respondToAuthRequestsFromMainProcess']);
    for (const key of Object.keys(options)) if (!allowed.has(key))
      throw error('ERR_NOT_SUPPORTED', 'Unsupported utilityProcess option: ' + key);
    const process = host.process;
    const cwd = path.resolve(options.cwd === undefined ? process.cwd() : text(options.cwd, 'cwd'));
    const entry = path.resolve(process.cwd(), modulePath);
    const inputEnv = options.env === undefined ? process.env : options.env;
    if (!inputEnv || typeof inputEnv !== 'object' || Array.isArray(inputEnv))
      throw invalid('env must be an object');
    const env = Object.create(null);
    for (const [key, value] of Object.entries(inputEnv)) {
      if (value === undefined) continue;
      if (!key || key.includes('=') || key.includes('\0'))
        throw error('ERR_INVALID_ARG_VALUE', 'Invalid environment variable name', TypeError);
      env[key] = text(String(value), 'environment value');
    }
    let stdio = options.stdio === undefined ? 'inherit' : options.stdio;
    if (typeof stdio === 'string') stdio = ['ignore', stdio, stdio];
    if (!Array.isArray(stdio) || stdio.length !== 3 || stdio[0] !== 'ignore' ||
        stdio.slice(1).some(mode => !['pipe', 'inherit', 'ignore'].includes(mode)))
      throw error('ERR_INVALID_ARG_VALUE', 'stdio requires ignored stdin and pipe, inherit or ignore outputs', TypeError);
    const config = {modulePath: entry, args, cwd, env,
      execArgv: strings(options.execArgv === undefined ? [] : options.execArgv, 'execArgv'),
      serviceName: options.serviceName === undefined ? 'Node Utility Process' : text(options.serviceName, 'serviceName'),
      stdout: stdio[1], stderr: stdio[2]};
    for (const name of ['allowLoadingUnsignedLibraries', 'respondToAuthRequestsFromMainProcess']) {
      if (options[name] !== undefined && typeof options[name] !== 'boolean')
        throw invalid(name + ' must be a boolean');
      config[name] = options[name] === true;
    }
    return config;
  }
  function release(state) {
    if (state.released || !state.exited || state.outputs.size) return;
    state.released = true;
    children.delete(state.id);
    host.__xenonUtilityControl(state.id, 'release');
  }
  function output(state, fd) {
    state.outputs.add(fd);
    let pending = false;
    const stream = new Readable({highWaterMark: 65536, read(size) {
      if (pending) return;
      pending = true;
      Promise.resolve().then(() => host.__xenonUtilityRead(state.id, fd,
        Math.min(Math.max(size, 1), 65536))).then(result => {
        pending = false;
        if (this.destroyed) return;
        if (result.data.byteLength) this.push(Buffer.from(result.data));
        if (result.eof) this.push(null);
        else if (!result.data.byteLength) this.destroy(error('EIO', 'Utility output read made no progress'));
      }, failure => { pending = false; if (!this.destroyed) this.destroy(failure); });
    }, destroy(failure, done) {
      try { host.__xenonUtilityControl(state.id, 'close-output', fd); }
      catch (closeError) { if (!failure) failure = closeError; }
      state.outputs.delete(fd);
      release(state);
      done(failure);
    }});
    return stream;
  }
  class UtilityProcess extends EventEmitter {
    constructor(token, config) {
      super();
      if (token !== secret) throw new TypeError('Illegal constructor');
      const channel = new MessageChannelMain();
      const state = {id: 0, port: channel.port1, pid: undefined, exited: false,
        outputs: new Set(), child: this};
      try {
        state.id = host.__xenonUtilityCreate(config, channel.port2);
        if (!Number.isInteger(state.id) || state.id < 1)
          throw error('ERR_INVALID_STATE', 'Host returned an invalid utility process identity');
      } catch (failure) {
        channel.port1.close(); channel.port2.close(); throw failure;
      }
      children.set(state.id, state);
      Object.defineProperty(this, 'pid', {enumerable: true, get: () => state.pid});
      this.stdout = config.stdout === 'pipe' ? output(state, 1) : null;
      this.stderr = config.stderr === 'pipe' ? output(state, 2) : null;
      state.port.on('message', event => this.emit('message', event.data));
      state.port.on('messageerror', () => this.emit('error', error('ERR_MESSAGE_DESERIALIZATION_FAILED', 'Utility message could not be decoded')));
      state.port.start();
      this.postMessage = (message, transfer) => {
        if (!state.exited) state.port.postMessage(message, transfer);
      };
      this.kill = () => !state.exited && host.__xenonUtilityControl(state.id, 'kill');
    }
  }
  host.__xenonDispatchUtilityProcessEvent = (id, type, value, message) => {
    const state = children.get(id);
    if (!state || state.exited) return;
    if (type === 'spawn') {
      state.pid = value;
      state.child.emit('spawn');
    } else if (type === 'error') {
      const code = /\b(ERR_[A-Z_]+|E[A-Z]+):/.exec(message || '')?.[1] || 'ERR_OPERATION_FAILED';
      state.child.emit('error', error(code, message || 'Utility process failed'));
    } else if (type === 'exit') {
      state.exited = true;
      state.pid = undefined;
      state.port.close();
      if (message === 'launch-failed' || message === 'disconnected') {
        state.child.stdout?.destroy();
        state.child.stderr?.destroy();
      }
      try { state.child.emit('exit', value); }
      finally {
        // Give exit listeners a chance to consume buffered bytes. Drain
        // untouched pipes so an unobserved stdout cannot retain the child;
        // a stream the application paused or consumed stays under its control.
        queueMicrotask(() => {
          for (const stream of [state.child.stdout, state.child.stderr]) {
            if (stream && !stream.destroyed && stream.readableFlowing === null &&
                !stream.readableDidRead && !stream.listenerCount('data') &&
                !stream.listenerCount('readable')) stream.resume();
          }
        });
        release(state);
      }
    }
  };
  return {fork(modulePath, args, options) {
    if (typeof host.__xenonUtilityCreate !== 'function')
      throw error('ERR_NOT_SUPPORTED', 'Native utilityProcess host is unavailable');
    return new UtilityProcess(secret, normalize(modulePath, args, options));
  }};
}

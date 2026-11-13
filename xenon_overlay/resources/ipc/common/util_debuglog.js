// debuglog lifecycle adapted from Node.js v24.19.0,
// lib/internal/util/debuglog.js. MIT license: see common/util_text.js.
// Copyright Node.js contributors. All rights reserved.
// Copyright Joyent, Inc. and other Node contributors. All rights reserved.
//
// Environment matching is fixed at runtime startup. Only section coercion,
// category logger creation, and the optional callback are lazy.
function createNodeDebuglog({debugEnv, getPid, format, write, emitWarning}) {
  const upper = Function.prototype.call.bind(String.prototype.toUpperCase);
  const lower = Function.prototype.call.bind(String.prototype.toLowerCase);
  const exec = Function.prototype.call.bind(RegExp.prototype.exec);
  const defineProperty = Object.defineProperty;
  const implementations = Object.create(null);
  const noop = () => {};
  let matches = () => false;
  if (debugEnv) {
    const expression = debugEnv.replace(/[|\\{}()[\]^$+?.]/g, '\\$&')
        .replaceAll('*', '.*').replaceAll(',', '$|^');
    const pattern = new RegExp(`^${expression}$`, 'i');
    matches = section => exec(pattern, section) !== null;
  }
  function implementation(enabled, section) {
    if (implementations[section] === undefined) {
      if (enabled) {
        const pid = getPid();
        if ((section === 'HTTP' || section === 'HTTP2') && emitWarning) {
          emitWarning('Setting the NODE_DEBUG environment variable ' +
              `to '${lower(section)}' can expose sensitive ` +
              'data (such as passwords, tokens and authentication headers) ' +
              'in the resulting log.');
        }
        implementations[section] = function debug(...args) {
          write(`${section} ${pid}: ${format(...args)}\n`);
        };
      } else {
        implementations[section] = noop;
      }
    }
    return implementations[section];
  }
  return function debuglog(section, callback) {
    let enabled;
    const init = () => {
      section = upper(section);
      enabled = matches(section);
    };
    let debug = (...args) => {
      init();
      debug = implementation(enabled, section);
      if (typeof callback === 'function') {
        defineProperty(debug, 'enabled', {
          __proto__: null,
          get: () => enabled, configurable: true, enumerable: true,
        });
        callback(debug);
      }
      return debug(...args);
    };
    let test = () => {
      init();
      test = () => enabled;
      return enabled;
    };
    const logger = (...args) => {
      if (enabled === false) return;
      return debug(...args);
    };
    defineProperty(logger, 'enabled', {
      __proto__: null,
      get: () => test(), configurable: true, enumerable: true,
    });
    return logger;
  };
}

// These runtimes have a synchronous host log sink, not a terminal. Keep stderr
// observable and writable (including user replacement), with no invented TTY.
function installNodeDebuglog(util, process, EventEmitter, Buffer, writeHost) {
  const stderr = new EventEmitter();
  Object.assign(stderr, {fd: 2, isTTY: false, writable: true});
  stderr.write = function write(chunk, encoding, callback) {
    if (typeof encoding === 'function') {
      callback = encoding;
      encoding = undefined;
    }
    if (callback !== undefined && typeof callback !== 'function') {
      throw Object.assign(new TypeError('The callback argument must be a function'),
          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (typeof chunk !== 'string' && !(chunk instanceof Uint8Array)) {
      throw Object.assign(new TypeError('The chunk argument must be a string or Uint8Array'),
          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    const text = typeof chunk === 'string' ? chunk : Buffer.from(chunk).toString('utf8');
    writeHost(text);
    if (callback) queueMicrotask(callback);
    return true;
  };
  process.stderr = stderr;
  const debuglog = createNodeDebuglog({
    debugEnv: process.env.NODE_DEBUG,
    getPid: () => process.pid,
    format: (...args) => util.format(...args),
    write: text => process.stderr.write(text),
    emitWarning: message => {
      const warning = new Error(message);
      warning.name = 'Warning';
      queueMicrotask(() => {
        process.emit('warning', warning);
        process.stderr.write(`(node:${process.pid}) Warning: ${message}\n`);
      });
    },
  });
  util.debug = debuglog;
  util.debuglog = debuglog;
}

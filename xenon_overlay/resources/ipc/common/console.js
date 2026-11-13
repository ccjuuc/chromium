// Console stream/error handling and timing contracts follow Node.js v24.
// Copyright Node.js contributors. All rights reserved.
// Copyright Joyent, Inc. and other Node contributors. All rights reserved.
// MIT license: see common/util_text.js.
// Formatting is deliberately delegated to the runtime's shared util module.
function createNodeConsole({util, stdout, stderr, now, emitWarning, env = {},
    publish = () => {}}) {
  const state = new WeakMap();
  const noop = () => {};
  const isMap = value => util.types?.isMap ? util.types.isMap(value) : value instanceof Map;
  const isSet = value => util.types?.isSet ? util.types.isSet(value) : value instanceof Set;
  const error = (Type, code, message) => Object.assign(new Type(message), {code});
  function optionsFor(instance, stream) {
    const s = state.get(instance);
    const options = isMap(s.inspectOptions)
        ? s.inspectOptions.get(stream) : s.inspectOptions;
    const colors = s.colorMode === 'auto'
        ? Boolean(stream.isTTY && (typeof stream.getColorDepth !== 'function' || stream.getColorDepth() > 2))
        : s.colorMode;
    if (options) {
      if (options.colors === undefined) options.colors = colors;
      return options;
    }
    return {colors};
  }
  function format(instance, stream, args) {
    if (!args.length) return '';
    if (args.length === 1 && typeof args[0] === 'string') return args[0];
    if (typeof util.formatWithOptions === 'function')
      return util.formatWithOptions(optionsFor(instance, stream), ...args);
    return util.format(...args);
  }
  function write(instance, stream, text) {
    const s = state.get(instance);
    if (s.indent) text = s.indent + text.replaceAll('\n', '\n' + s.indent);
    text += '\n';
    if (!instance._ignoreErrors) { stream.write(text); return; }
    // Writable may emit a synchronous error, or schedule one after its write
    // callback. Only suppress errors from this console write, not future writes.
    const handleError = err => {
      if (err && !stream._writableState?.errorEmitted && stream.listenerCount?.('error') === 0)
        stream.once('error', noop);
    };
    try {
      if (stream.listenerCount?.('error') === 0) stream.once('error', noop);
      stream.write(text, handleError);
    } catch (e) {
      if (e instanceof RangeError && /call stack/i.test(e.message)) throw e;
    } finally { stream.removeListener?.('error', noop); }
  }
  function duration(ms) {
    if (ms < 1000) return `${Number(ms.toFixed(3))}ms`;
    if (ms < 60000) return `${(ms / 1000).toFixed(3)}s`;
    const hours = Math.floor(ms / 3600000);
    const minutes = Math.floor(ms % 3600000 / 60000);
    const seconds = (ms % 60000 / 1000).toFixed(3).padStart(6, '0');
    return (hours ? `${hours}:${String(minutes).padStart(2, '0')}` : minutes) +
        `:${seconds} (${hours ? 'h:m' : ''}m:ss.mmm)`;
  }
  function timer(instance, label, method, data) {
    label = `${label}`;
    const times = state.get(instance).times;
    if (!times.has(label)) {
      emitWarning(`No such label '${label}' for console.${method}()`);
      return;
    }
    instance.log('%s: %s', label, duration(now() - times.get(label)), ...data);
    if (method === 'timeEnd') times.delete(label);
  }
  function Console(options) {
    if (!new.target) return Reflect.construct(Console, arguments);
    if (!options || typeof options.write === 'function')
      options = {stdout: options, stderr: arguments[1], ignoreErrors: arguments[2]};
    const {stdout, stderr = stdout, ignoreErrors = true, colorMode = 'auto',
      inspectOptions, groupIndentation = 2} = options;
    for (const [name, stream] of [['stdout', stdout], ['stderr', stderr]])
      if (!stream || typeof stream.write !== 'function')
        throw error(TypeError, 'ERR_CONSOLE_WRITABLE_STREAM', `Console expects a writable stream instance for ${name}`);
    if (colorMode !== 'auto' && colorMode !== true && colorMode !== false)
      throw error(TypeError, 'ERR_INVALID_ARG_VALUE', 'Invalid colorMode');
    if (typeof groupIndentation !== 'number')
      throw error(TypeError, 'ERR_INVALID_ARG_TYPE', 'groupIndentation must be a number');
    if (!Number.isInteger(groupIndentation) || groupIndentation < 0 || groupIndentation > 1000)
      throw error(RangeError, 'ERR_OUT_OF_RANGE', 'groupIndentation must be an integer between 0 and 1000');
    if (inspectOptions !== undefined) {
      if (!inspectOptions || typeof inspectOptions !== 'object' || Array.isArray(inspectOptions))
        throw error(TypeError, 'ERR_INVALID_ARG_TYPE', 'inspectOptions must be an object');
      for (const item of isMap(inspectOptions) ? inspectOptions.values() : [inspectOptions])
        if (item.colors !== undefined && options.colorMode !== undefined)
          throw error(TypeError, 'ERR_INCOMPATIBLE_OPTION_PAIR', 'Cannot use both colorMode and inspectOptions.colors');
    }
    state.set(this, {indent: '', width: groupIndentation, colorMode, inspectOptions,
      times: new Map(), counts: new Map()});
    Object.defineProperties(this, {
      _stdout: {value: stdout, writable: true, configurable: true},
      _stderr: {value: stderr, writable: true, configurable: true},
      _ignoreErrors: {value: Boolean(ignoreErrors), writable: true, configurable: true},
    });
    for (const name of Object.keys(Console.prototype)) {
      this[name] = this[name].bind(this);
      Object.defineProperty(this[name], 'name', {value: name, configurable: true});
    }
  }
  for (const name of ['log', 'info', 'debug', 'warn', 'error']) {
    Console.prototype[name] = function(...args) {
      publish(name, args);
      const stream = name === 'warn' || name === 'error' ? this._stderr : this._stdout;
      write(this, stream, format(this, stream, args));
    };
  }
  Object.assign(Console.prototype, {
    dir(value, options) {
      write(this, this._stdout, util.inspect(value,
          {customInspect: false, ...optionsFor(this, this._stdout), ...options}));
    },
    time(label = 'default') {
      label = `${label}`;
      const times = state.get(this).times;
      if (times.has(label)) { emitWarning(`Label '${label}' already exists for console.time()`); return; }
      times.set(label, now());
    },
    timeEnd(label = 'default') { timer(this, label, 'timeEnd', []); },
    timeLog(label = 'default', ...data) { timer(this, label, 'timeLog', data); },
    trace: function trace(...args) {
      const err = {name: 'Trace', message: format(this, this._stderr, args)};
      Error.captureStackTrace(err, trace);
      this.error(err.stack);
    },
    assert(expression, ...args) {
      if (expression) return;
      if (args.length && typeof args[0] === 'string') args[0] = 'Assertion failed: ' + args[0];
      else args.unshift('Assertion failed');
      this.warn(...args);
    },
    clear() {
      if (this._stdout.isTTY && env.TERM !== 'dumb') this._stdout.write('\x1b[1;1H\x1b[0J');
    },
    count(label = 'default') {
      label = `${label}`;
      const counts = state.get(this).counts;
      const count = (counts.get(label) || 0) + 1;
      counts.set(label, count);
      this.log(`${label}: ${count}`);
    },
    countReset(label = 'default') {
      const counts = state.get(this).counts;
      if (!counts.has(label)) { emitWarning(`Count for '${label}' does not exist`); return; }
      counts.delete(`${label}`);
    },
    group(...data) {
      if (data.length) this.log(...data);
      const s = state.get(this);
      s.indent += ' '.repeat(s.width);
    },
    groupEnd() {
      const s = state.get(this);
      s.indent = s.indent.slice(0, Math.max(0, s.indent.length - s.width));
    },
    table(data, properties) {
      if (properties !== undefined && !Array.isArray(properties))
        throw error(TypeError, 'ERR_INVALID_ARG_TYPE', 'properties must be an Array');
      if (data === null || typeof data !== 'object') { this.log(data); return; }
      const inspect = value => util.inspect(value, {depth: value && typeof value === 'object' &&
        !Array.isArray(value) && Object.keys(value).length > 2 ? -1 : 0,
        maxArrayLength: 3, breakLength: Infinity, ...optionsFor(this, this._stdout)});
      let headings, rows;
      if (isMap(data) || isSet(data)) {
        const map = isMap(data);
        headings = map ? ['(iteration index)', 'Key', 'Values'] : ['(iteration index)', 'Values'];
        rows = [...data].map((value, i) => map
            ? [String(i), inspect(value[0]), inspect(value[1])] : [String(i), inspect(value)]);
      } else {
        const keys = Object.keys(data), columns = new Set(properties || []);
        let hasValues = false;
        const values = keys.map(key => {
          const value = data[key];
          const object = value !== null && (typeof value === 'object' || typeof value === 'function');
          if (!object && !properties) hasValues = true;
          if (object && !properties) Object.keys(value).forEach(key => columns.add(key));
          return {value, object};
        });
        headings = ['(index)', ...columns, ...(hasValues ? ['Values'] : [])];
        rows = values.map(({value, object}, i) => [keys[i], ...[...columns].map(key =>
          object && Object.prototype.hasOwnProperty.call(value, key) ? inspect(value[key]) : ''),
          ...(hasValues ? [object ? '' : inspect(value)] : [])]);
      }
      const width = text => [...(util.stripVTControlCharacters?.(String(text)) || String(text))].length;
      const widths = headings.map((h, i) => Math.max(width(h), ...rows.map(row => width(row[i]))));
      const border = (left, mid, right) => left + widths.map(w => '─'.repeat(w + 2)).join(mid) + right;
      const row = cells => '│ ' + cells.map((cell, i) => cell + ' '.repeat(widths[i] - width(cell))).join(' │ ') + ' │';
      this.log([border('┌','┬','┐'), row(headings), border('├','┼','┤'),
        ...rows.map(row), border('└','┴','┘')].join('\n'));
    },
  });
  Console.prototype.dirxml = Console.prototype.log;
  Console.prototype.groupCollapsed = Console.prototype.group;
  const instance = new Console({stdout, stderr});
  instance.Console = Console;
  return instance;
}

// This transport reports bytes to the actual host sink. It is not a terminal,
// pipe, or placeholder successful write; future utility-child OS stdio needs
// its own stream transport so native fd writes are also observable.
function createConsoleHostStream({EventEmitter, Buffer, fd, writeHost}) {
  const stream = new EventEmitter();
  Object.assign(stream, {fd, isTTY: false, writable: true});
  stream.write = function write(chunk, encoding, callback) {
    if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
    if (callback !== undefined && typeof callback !== 'function')
      throw Object.assign(new TypeError('The callback argument must be a function'), {code: 'ERR_INVALID_ARG_TYPE'});
    if (typeof chunk !== 'string' && !(chunk instanceof Uint8Array))
      throw Object.assign(new TypeError('The chunk argument must be a string or Uint8Array'), {code: 'ERR_INVALID_ARG_TYPE'});
    writeHost(typeof chunk === 'string' ? chunk : Buffer.from(chunk).toString('utf8'));
    if (callback) queueMicrotask(() => callback(null));
    return true;
  };
  return stream;
}

function installNodeConsole({util, process, EventEmitter, Buffer, writeHost, now,
    publish}) {
  process.stdout = createConsoleHostStream({EventEmitter, Buffer, fd: 1, writeHost});
  const consoleModule = createNodeConsole({util, stdout: process.stdout,
    stderr: process.stderr, now, env: process.env, publish,
    emitWarning: message => {
      if (typeof process.emitWarning === 'function') { process.emitWarning(message); return; }
      const warning = new Error(message);
      warning.name = 'Warning';
      queueMicrotask(() => {
        process.emit('warning', warning);
        process.stderr.write(`(node:${process.pid}) Warning: ${message}\n`);
      });
    },
  });
  // Node's global Console resolves the process streams lazily. Application
  // redirection of process.stdout/stderr must also affect require('console').
  Object.defineProperties(consoleModule, {
    _stdout: {get: () => process.stdout, configurable: true},
    _stderr: {get: () => process.stderr, configurable: true},
  });
  return consoleModule;
}

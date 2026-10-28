  // The macOS SDK owns AppKit views and therefore runs in the Browser process.
  // This optional, versioned capability leaves ordinary Electron/Windows addon
  // loading unchanged. The Browser independently validates the bundled addon,
  // caller generation, native handles and ownership of every attached window.
  if (__xenonPlatform === 'darwin' &&
      typeof __xenonBrowserWindowCall === 'function') {
    let macPlayerCapability;
    let macPlayerCapabilityProbed = false;
    function createMacPlayerCapability() {
      const wireKey = '__xenon_node_wire_type__';
      const handles = new WeakMap();
      const proxies = new Map();
      const callbacks = new Map();
      const typedArrays = new Map([
        'Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array',
        'Uint16Array', 'Int32Array', 'Uint32Array', 'Float16Array',
        'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array',
      ].filter(name => typeof globalThis[name] === 'function')
          .map(name => [name, globalThis[name]]));
      let generation = 0;
      let nextCallbackId = 1;
      let addonPath = '';
      let addon;

      function fail(message, code = 'ERR_INVALID_ARG_TYPE') {
        throw Object.assign(new TypeError(message), {code});
      }
      function checkGeneration(expected) {
        if (expected !== generation) {
          fail('Mac player runtime was closed', 'ERR_INVALID_STATE');
        }
      }
      function call(request, expected = generation, callbackIds = []) {
        checkGeneration(expected);
        try {
          const reply = __xenonBrowserWindowCall(0, 'mac-player',
              callbackIds.length ? {...request, callbackIds} : request);
          // A transport exception has unknown acceptance: native code may have
          // retained callbacks before the reply was lost. Only an explicit,
          // valid host acknowledgement can release this request's callbacks.
          if (!reply || reply.protocolVersion !== 1 ||
              typeof reply.error !== 'string' || !Object.hasOwn(reply, 'result') ||
              !Array.isArray(reply.acceptedCallbackIds)) {
            fail('Invalid Mac player callback acknowledgement', 'ERR_INVALID_STATE');
          }
          const allocated = new Set(callbackIds);
          const accepted = new Set();
          for (const id of reply.acceptedCallbackIds) {
            if (!Number.isInteger(id) || !allocated.has(id) || accepted.has(id)) {
              fail('Invalid Mac player callback acknowledgement', 'ERR_INVALID_STATE');
            }
            accepted.add(id);
          }
          for (const id of allocated) {
            if (!accepted.has(id)) callbacks.delete(id);
          }
          if (reply.error) throw new Error(reply.error);
          return reply.result;
        } catch (error) {
          const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
          if (code && !error.code) error.code = code[1];
          throw error;
        }
      }
      const finalizer = new FinalizationRegistry(record => {
        if (record.generation !== generation) return;
        const cached = proxies.get(record.key);
        // A replacement wrapper may acquire the same native object before an
        // earlier wrapper's finalizer runs. Its lease must remain live.
        if (cached && cached.record !== record) return;
        proxies.delete(record.key);
        try {
          call({operation: 'release', instanceId: record.instanceId,
            token: record.token}, record.generation);
        } catch (_) {
          // The Browser reclaims the owner on disconnect. GC cannot reconnect.
        }
      });

      function prepare(args) {
        const seen = new Set();
        const allocated = [];
        let count = 0;
        function visit(value, depth) {
          if (++count > 10000 || depth > 32) {
            fail('Mac player arguments exceed the serialization limit',
                'ERR_OUT_OF_RANGE');
          }
          if (value === undefined) return {[wireKey]: 'undefined'};
          if (typeof value === 'bigint') {
            return {[wireKey]: 'bigint', value: String(value)};
          }
          if (typeof value === 'number' &&
              (!Number.isFinite(value) || Object.is(value, -0))) {
            return {[wireKey]: 'number', value: Number.isNaN(value) ? 'nan' :
              value === Infinity ? 'infinity' :
              value === -Infinity ? '-infinity' : '-0'};
          }
          if (value === null || ['string', 'number', 'boolean'].includes(
              typeof value)) return value;
          const handle = handles.get(value);
          if (handle) {
            checkGeneration(handle.generation);
            if (handle.consumed) {
              fail('Native player completion was already called', 'ERR_INVALID_STATE');
            }
            return {[wireKey]: handle.kind, instance_id: handle.instanceId,
              module_path: handle.modulePath, owner_token: handle.token};
          }
          if (typeof value === 'function') {
            if (callbacks.size >= 4096 || nextCallbackId > 2147483647) {
              fail('Too many retained Mac player callbacks', 'ERR_OUT_OF_RANGE');
            }
            const callbackId = nextCallbackId++;
            callbacks.set(callbackId, captureAsyncCallback(value));
            allocated.push(callbackId);
            return {[wireKey]: 'callback', callback_id: callbackId};
          }
          if (typeof value !== 'object') {
            fail('Unsupported Mac player argument type: ' + typeof value);
          }
          if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) {
            const kind = Buffer.isBuffer(value) ? 'Buffer' :
              value instanceof ArrayBuffer ? 'ArrayBuffer' :
              value instanceof DataView ? 'DataView' :
              Object.prototype.toString.call(value).slice(8, -1);
            if (!['Buffer', 'ArrayBuffer', 'DataView'].includes(kind) &&
                !typedArrays.has(kind)) fail('Unsupported native binary kind');
            const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) :
              new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
            return {[wireKey]: 'binary', kind, value: bytes};
          }
          if (value instanceof Date) {
            return {[wireKey]: 'date', value: visit(value.getTime(), depth + 1)};
          }
          if (seen.has(value)) fail('Circular Mac player argument');
          seen.add(value);
          try {
            if (Array.isArray(value)) {
              return Array.from(value, item => visit(item, depth + 1));
            }
            if (Object.prototype.toString.call(value) !== '[object Object]') {
              fail('Unsupported Mac player argument object');
            }
            if (Object.hasOwn(value, wireKey)) {
              fail('Reserved Mac player wire property');
            }
            const result = {};
            for (const key of Object.keys(value)) {
              Object.defineProperty(result, key, {enumerable: true,
                configurable: true, writable: true,
                value: visit(value[key], depth + 1)});
            }
            return result;
          } finally {
            seen.delete(value);
          }
        }
        try {
          return {args: visit(args, 0), callbackIds: allocated};
        } catch (error) {
          // No request was sent, so these callbacks have no native owner.
          for (const id of allocated) callbacks.delete(id);
          throw error;
        }
      }

      function binary(value) {
        const bytes = value.value;
        if (!ArrayBuffer.isView(bytes)) fail('Invalid native binary result');
        const buffer = bytes.buffer.slice(
            bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
        if (value.kind === 'Buffer') return Buffer.from(buffer);
        if (value.kind === 'ArrayBuffer') return buffer;
        if (value.kind === 'DataView') return new DataView(buffer);
        const Type = typedArrays.get(value.kind);
        if (!Type || buffer.byteLength % Type.BYTES_PER_ELEMENT) {
          fail('Invalid native binary kind or length');
        }
        return new Type(buffer);
      }
      function adopt(value) {
        if (!value || typeof value !== 'object') return value;
        if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer) return value;
        if (Array.isArray(value)) return value.map(adopt);
        switch (value[wireKey]) {
          case 'undefined': return undefined;
          case 'global': return globalThis;
          case 'bigint': return BigInt(value.value);
          case 'number': {
            const numbers = {nan: NaN, infinity: Infinity, '-infinity': -Infinity,
              '-0': -0};
            if (!Object.hasOwn(numbers, value.value)) fail('Invalid native number');
            return numbers[value.value];
          }
          case 'date': return new Date(adopt(value.value));
          case 'map': return new Map(value.value.map(pair => pair.map(adopt)));
          case 'set': return new Set(value.value.map(adopt));
          case 'binary': return binary(value);
          case 'native_instance':
          case 'native_function': return instance(value);
          case undefined: break;
          default: fail('Unsupported native result wire type');
        }
        const result = {};
        for (const key of Object.keys(value)) {
          Object.defineProperty(result, key, {enumerable: true,
            configurable: true, writable: true, value: adopt(value[key])});
        }
        return result;
      }
      function invokeInstance(handle, method, args, prepared = false) {
        checkGeneration(handle.generation);
        const transaction = prepared ? args : prepare(args);
        return adopt(call({operation: 'invoke-instance',
          instanceId: handle.instanceId, token: handle.token,
          method, args: transaction.args},
        handle.generation, transaction.callbackIds));
      }
      function instance(value) {
        if (!Number.isInteger(value.instance_id) || value.instance_id <= 0 ||
            typeof value.owner_token !== 'string' || !value.owner_token ||
            typeof value.module_path !== 'string' || !value.module_path) {
          fail('Invalid native player handle');
        }
        const key = JSON.stringify([
          generation, value.module_path, value.owner_token, value.instance_id]);
        const cached = proxies.get(key);
        const previous = cached?.proxy.deref();
        if (previous) return previous;
        // Extracted methods keep the handle alive even if the wrapper is gone.
        const handle = cached?.handle.deref() || {
          generation, modulePath: value.module_path, token: value.owner_token,
          instanceId: value.instance_id, kind: value[wireKey], consumed: false,
        };
        const target = handle.kind === 'native_function' ? function(...args) {
          checkGeneration(handle.generation);
          if (handle.consumed) {
            fail('Native player completion was already called', 'ERR_INVALID_STATE');
          }
          // SDK HTTP completions consume their native userdata. Reusing one
          // would dereference freed memory even while its JS function is alive.
          const prepared = prepare([undefined, ...args]);
          handle.consumed = true;
          return invokeInstance(handle, 'call', prepared, true);
        } : {};
        handles.set(target, handle);
        if (!handle.record) {
          handle.record = Object.freeze({key, generation: handle.generation,
            instanceId: handle.instanceId, token: handle.token});
          finalizer.register(handle, handle.record);
        }
        proxies.set(key, {proxy: new WeakRef(target), handle: new WeakRef(handle),
          record: handle.record});
        if (handle.kind === 'native_function') return target;
        for (const [name, field] of Object.entries(value.fields || {})) {
          Object.defineProperty(target, name, {enumerable: true,
            configurable: true, writable: true, value: adopt(field)});
        }
        for (const member of value.prototype || []) {
          if (!member || typeof member.name !== 'string') continue;
          if (member.kind !== 'function' && member.kind !== 'class') continue;
          Object.defineProperty(target, member.name, {
            enumerable: !!member.enumerable, configurable: true, writable: true,
            value: (...args) => invokeInstance(handle, member.name, args),
          });
        }
        return target;
      }

      function createExports(root, expected) {
        const exports = new Map();
        function describe(path) {
          return call({operation: 'describe', exportPath: path}, expected);
        }
        function build(info, path, cache = true) {
          checkGeneration(expected);
          if (!info || info.kind === 'undefined') return undefined;
          if (info.kind === 'null') return null;
          if (info.kind === 'bigint') return BigInt(info.value);
          if (info.hasValue) return adopt(info.value);
          if (!['object', 'array', 'function', 'class'].includes(info.kind)) {
            fail('Unsupported native export kind: ' + info.kind);
          }
          if (cache && exports.has(path)) return exports.get(path);
          const callable = info.kind === 'function' || info.kind === 'class';
          const target = callable ? function(...args) {
            checkGeneration(expected);
            const transaction = prepare(args);
            const value = adopt(call({operation: new.target ? 'construct' : 'invoke',
              exportPath: path, args: transaction.args}, expected,
            transaction.callbackIds));
            if (new.target && target.prototype &&
                typeof target.prototype === 'object') {
              Object.setPrototypeOf(value, target.prototype);
            }
            return value;
          }.bind(null) : info.kind === 'array' ? [] : {};
          if (cache) exports.set(path, target);
          if (callable) {
            Object.defineProperty(target, Symbol.hasInstance, {value(value) {
              return target.prototype && typeof target.prototype === 'object' &&
                Object.prototype.isPrototypeOf.call(target.prototype, value);
            }});
          }
          for (const child of info.children || []) {
            const name = child.name;
            if (typeof name !== 'string' || !name || name.includes('.')) {
              fail('Invalid native export property path');
            }
            const childPath = path ? path + '.' + name : name;
            const attributes = {enumerable: !!child.enumerable, configurable: true};
            if (Array.isArray(target) && name === 'length') {
              target.length = child.value;
            } else if (child.kind === 'property') {
              Object.defineProperty(target, name, {...attributes, get() {
                const property = describe(childPath);
                if (['object', 'array', 'function', 'class'].includes(property.kind)) {
                  fail('Native accessor objects require a retained handle');
                }
                return build(property, childPath, false);
              }});
            } else if (['object', 'array', 'function', 'class'].includes(child.kind)) {
              Object.defineProperty(target, name, {...attributes, get() {
                const value = build(describe(childPath), childPath, cache);
                Object.defineProperty(target, name, {...attributes, value,
                  writable: !!child.writable});
                return value;
              }});
            } else {
              Object.defineProperty(target, name, {...attributes,
                value: build(child, childPath, cache), writable: !!child.writable});
            }
          }
          return target;
        }
        return build(root, '');
      }

      globalThis.__xenonDispatchMacPlayerEvent = details => {
        if (!details || typeof details !== 'object') return;
        if (details.type === 'reset') {
          generation++;
          callbacks.clear();
          proxies.clear();
          addon = undefined;
          addonPath = '';
        } else if (details.type === 'release-callback') {
          callbacks.delete(details.callbackId);
        } else if (details.type === 'callback' && Array.isArray(details.args)) {
          const callback = callbacks.get(details.callbackId);
          if (!callback) return;
          try {
            Reflect.apply(callback, adopt(details.receiver), details.args.map(adopt));
          } catch (error) {
            console.error('[Xenon Mac Player Callback]', error);
          }
        }
      };
      function windowCall(operation, windowId) {
        if (!Number.isInteger(windowId) || windowId <= 0 || windowId > 2147483647) {
          fail('Mac player requires a valid BrowserWindow ID');
        }
        return call({operation, windowId});
      }
      const descriptor = call({operation: 'capability'});
      if (descriptor === null) return undefined;
      if (!descriptor || descriptor.version !== 1 ||
          typeof descriptor.adapterId !== 'string' || !descriptor.adapterId ||
          !Number.isInteger(descriptor.adapterVersion) || descriptor.adapterVersion <= 0) {
        fail('Invalid Mac player capability descriptor', 'ERR_INVALID_STATE');
      }
      return Object.freeze({
        version: 1,
        adapterId: descriptor.adapterId,
        adapterVersion: descriptor.adapterVersion,
        getAddon(path) {
          if (typeof path !== 'string' || !path || path.includes('\0')) {
            fail('Mac player requires an addon path');
          }
          if (addon) {
            if (path !== addonPath) fail('Mac player addon is already loaded');
            return addon;
          }
          const loaded = createExports(call({operation: 'load', path}), generation);
          addonPath = path;
          addon = loaded;
          return addon;
        },
        attachWindow: windowId => windowCall('attach', windowId),
        detachWindow: windowId => windowCall('detach', windowId),
      });
    }
    Object.defineProperty(electronModule, 'xenonMacPlayer', {
      enumerable: true,
      get() {
        if (macPlayerCapabilityProbed) return macPlayerCapability;
        macPlayerCapability = createMacPlayerCapability();
        macPlayerCapabilityProbed = true;
        return macPlayerCapability;
      },
    });
  }

  // Channels are local to one Node context. Inactive channels are weakly
  // retained; subscriptions and stores keep their named channel alive.
  function createDiagnosticsChannel(reportError) {
    const channels = new Map();
    const states = new WeakMap();
    const invalid = name => Object.assign(new TypeError(name + ' has an invalid type'),
      {code: 'ERR_INVALID_ARG_TYPE'});
    const finalize = new FinalizationRegistry(({name, record}) => {
      if (channels.get(name) === record && !record.reference.deref()) channels.delete(name);
    });
    const getChannel = name => {
      const record = channels.get(name);
      return record?.strong || record?.reference.deref();
    };
    function activate(channel) {
      const state = states.get(channel);
      state.subscribers ??= [];
      state.stores ??= new Map();
      const record = channels.get(channel.name);
      if (record?.reference.deref() === channel) record.strong = channel;
      return state;
    }
    function release(channel) {
      const state = states.get(channel);
      if (state.subscribers.length || state.stores.size) return;
      state.subscribers = undefined;
      state.stores = undefined;
      const record = channels.get(channel.name);
      if (record?.strong === channel) record.strong = undefined;
    }
    class Channel {
      static [Symbol.hasInstance](value) { return Object.getPrototypeOf(value) === Channel.prototype; }
      constructor(name) {
        this.name = name;
        states.set(this, {subscribers: undefined, stores: undefined});
        const record = {reference: new WeakRef(this), strong: undefined};
        channels.set(name, record);
        finalize.register(this, {name, record});
      }
      subscribe(subscription) {
        const state = activate(this);
        if (typeof subscription !== 'function') throw invalid('subscription');
        state.subscribers = [...state.subscribers, subscription];
      }
      unsubscribe(subscription) {
        const state = states.get(this);
        const index = state.subscribers?.indexOf(subscription) ?? -1;
        if (index < 0) return false;
        state.subscribers = state.subscribers.filter((_item, position) => position !== index);
        release(this);
        return true;
      }
      get hasSubscribers() { return states.get(this).subscribers !== undefined; }
      publish(data) {
        // Copy-on-write subscription updates preserve the current publication
        // when a callback subscribes/unsubscribes or publishes recursively.
        const subscriptions = states.get(this).subscribers;
        for (const subscriber of subscriptions || []) {
          try { subscriber(data, this.name); }
          catch (error) { reportError(error); }
        }
      }
      bindStore(store, transform) { activate(this).stores.set(store, transform); }
      unbindStore(store) {
        const state = states.get(this);
        if (!state.stores?.delete(store)) return false;
        release(this);
        return true;
      }
      runStores(data, fn, thisArg, ...args) {
        const state = states.get(this);
        if (!state.stores) return Reflect.apply(fn, thisArg, args);
        let run = () => { this.publish(data); return Reflect.apply(fn, thisArg, args); };
        for (const [store, transform] of state.stores) {
          const next = run;
          run = () => {
            let value;
            try { value = transform === undefined ? data : transform(data); }
            catch (error) { reportError(error); return next(); }
            return store.run(value, next);
          };
        }
        return run();
      }
    }
    function channel(name) {
      const existing = getChannel(name);
      if (existing) return existing;
      if (typeof name !== 'string' && typeof name !== 'symbol') throw invalid('channel');
      return new Channel(name);
    }
    const traceEvents = ['start', 'end', 'asyncStart', 'asyncEnd', 'error'];
    class TracingChannel {
      constructor(nameOrChannels) {
        for (const name of traceEvents) {
          let selected;
          if (typeof nameOrChannels === 'string') selected = channel('tracing:' + nameOrChannels + ':' + name);
          else {
            if (!nameOrChannels || typeof nameOrChannels !== 'object') throw invalid('nameOrChannels');
            selected = nameOrChannels[name];
            if (!(selected instanceof Channel)) throw invalid('nameOrChannels.' + name);
          }
          Object.defineProperty(this, name, {value: selected});
        }
      }
      get hasSubscribers() { return traceEvents.some(name => this[name]?.hasSubscribers); }
      subscribe(handlers) {
        for (const name of traceEvents) if (handlers[name]) this[name]?.subscribe(handlers[name]);
      }
      unsubscribe(handlers) {
        let removed = true;
        for (const name of traceEvents) if (handlers[name] && !this[name]?.unsubscribe(handlers[name])) removed = false;
        return removed;
      }
      traceSync(fn, context = {}, thisArg, ...args) {
        if (!this.hasSubscribers) return Reflect.apply(fn, thisArg, args);
        return this.start.runStores(context, () => {
          try { return context.result = Reflect.apply(fn, thisArg, args); }
          catch (error) { context.error = error; this.error.publish(context); throw error; }
          finally { this.end.publish(context); }
        });
      }
      tracePromise(fn, context = {}, thisArg, ...args) {
        if (!this.hasSubscribers) return Reflect.apply(fn, thisArg, args);
        return this.start.runStores(context, () => {
          try {
            const promise = Reflect.apply(fn, thisArg, args);
            return Promise.resolve(promise).then(result => {
              context.result = result;
              this.asyncStart.publish(context);
              this.asyncEnd.publish(context);
              return result;
            }, error => {
              context.error = error;
              this.error.publish(context);
              this.asyncStart.publish(context);
              this.asyncEnd.publish(context);
              throw error;
            });
          } catch (error) { context.error = error; this.error.publish(context); throw error; }
          finally { this.end.publish(context); }
        });
      }
      traceCallback(fn, position = -1, context = {}, thisArg, ...args) {
        if (!this.hasSubscribers) return Reflect.apply(fn, thisArg, args);
        const callback = args.at(position);
        if (typeof callback !== 'function') throw invalid('callback');
        const trace = this;
        args.splice(position, 1, function(error, result, ...rest) {
          if (error) { context.error = error; trace.error.publish(context); }
          else context.result = result;
          const callbackArgs = arguments;
          trace.asyncStart.runStores(context, () => {
            try { return Reflect.apply(callback, this, callbackArgs); }
            finally { trace.asyncEnd.publish(context); }
          });
        });
        return this.start.runStores(context, () => {
          try { return Reflect.apply(fn, thisArg, args); }
          catch (error) { context.error = error; this.error.publish(context); throw error; }
          finally { this.end.publish(context); }
        });
      }
    }
    return {Channel, channel,
      subscribe: (name, subscriber) => channel(name).subscribe(subscriber),
      unsubscribe: (name, subscriber) => channel(name).unsubscribe(subscriber),
      hasSubscribers: name => getChannel(name)?.hasSubscribers || false,
      tracingChannel: name => new TracingChannel(name),
    };
  }

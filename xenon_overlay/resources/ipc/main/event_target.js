  // Main runs in a plain V8 context: it has no Blink event objects. These
  // classes implement Node's single-target event model, independently of the
  // EventEmitter module. Renderer contexts continue using Blink's classes.
  function createMainEventClasses(reportError, now) {
    const eventStates = new WeakMap();
    const targetStates = new WeakMap();
    const customDetails = new WeakMap();
    const resistStop = Symbol('internal abort cleanup');
    const invalid = (code, message, Constructor = TypeError) =>
      Object.assign(new Constructor(message), {code});
    function stateFor(states, receiver, name) {
      const state = states.get(receiver);
      if (!state) throw invalid('ERR_INVALID_THIS', `Value of this must be ${name}`);
      return state;
    }
    function eventState(receiver) {
      return stateFor(eventStates, receiver, 'Event');
    }
    function requireArguments(actual, minimum) {
      if (actual < minimum)
        throw invalid('ERR_MISSING_ARGS', 'Required event argument is missing');
    }
    function dictionary(value) {
      if (value == null) return {};
      if (typeof value !== 'object')
        throw invalid('ERR_INVALID_ARG_TYPE', 'Event options must be an object');
      return value;
    }
    function validListener(listener) {
      if (listener == null) return false;
      if (typeof listener === 'function' || typeof listener === 'object') return true;
      throw invalid('ERR_INVALID_ARG_TYPE', 'Listener must be a function or an object');
    }
    class Event {
      constructor(type, options) {
        requireArguments(arguments.length, 1);
        options = dictionary(options);
        eventStates.set(this, {
          type: `${type}`, bubbles: !!options.bubbles,
          cancelable: !!options.cancelable, composed: !!options.composed,
          canceled: false, stopped: false, immediate: false, passive: false,
          dispatching: false, target: null, timeStamp: now(),
        });
      }
      get type() { return eventState(this).type; }
      get bubbles() { return eventState(this).bubbles; }
      get cancelable() { return eventState(this).cancelable; }
      get composed() { return eventState(this).composed; }
      get timeStamp() { return eventState(this).timeStamp; }
      get target() { return eventState(this).target; }
      get srcElement() { return eventState(this).target; }
      get currentTarget() {
        const state = eventState(this);
        return state.dispatching ? state.target : null;
      }
      get eventPhase() { return eventState(this).dispatching ? 2 : 0; }
      get isTrusted() { eventState(this); return false; }
      get defaultPrevented() {
        const state = eventState(this);
        return state.cancelable && state.canceled;
      }
      get returnValue() { return !this.defaultPrevented; }
      set returnValue(value) { eventState(this); if (!value) this.preventDefault(); }
      get cancelBubble() { return eventState(this).stopped; }
      set cancelBubble(value) { eventState(this); if (value) this.stopPropagation(); }
      preventDefault() {
        const state = eventState(this);
        if (state.cancelable && !state.passive) state.canceled = true;
      }
      stopPropagation() { eventState(this).stopped = true; }
      stopImmediatePropagation() {
        const state = eventState(this);
        state.stopped = state.immediate = true;
      }
      composedPath() {
        const state = eventState(this);
        return state.dispatching ? [state.target] : [];
      }
      initEvent(type, bubbles = false, cancelable = false) {
        requireArguments(arguments.length, 1);
        const state = eventState(this);
        if (state.dispatching) return;
        state.type = `${type}`;
        state.bubbles = !!bubbles;
        state.cancelable = !!cancelable;
      }
    }
    class CustomEvent extends Event {
      constructor(type, options) {
        requireArguments(arguments.length, 1);
        options = dictionary(options);
        super(type, options);
        customDetails.set(this, {detail: options.detail ?? null});
      }
      get detail() {
        return stateFor(customDetails, this, 'CustomEvent').detail;
      }
    }
    function detach(map, type, root, entry) {
      if (entry.removed) return;
      entry.removed = true;
      entry.previous.next = entry.next;
      if (entry.next) entry.next.previous = entry.previous;
      else root.tail = entry.previous;
      // Preserve entry.next for an in-progress dispatch that already captured
      // this node. A removed/re-added listener must not become that old node.
      if (!root.next) map.delete(type);
      if (entry.signal) {
        entry.signal.removeEventListener('abort', entry.abort);
        entry.signal = entry.abort = null;
      }
    }
    class EventTarget {
      constructor() { targetStates.set(this, new Map()); }
      addEventListener(type, listener, options = {}) {
        const map = stateFor(targetStates, this, 'EventTarget');
        requireArguments(arguments.length, 2);
        options = typeof options === 'boolean' ? {capture: options} : dictionary(options);
        const once = !!options.once;
        const capture = !!options.capture;
        const passive = !!options.passive;
        const signal = options.signal;
        if (signal !== undefined && (signal === null ||
            typeof signal !== 'object' || typeof signal.aborted !== 'boolean' ||
            typeof signal.addEventListener !== 'function' ||
            typeof signal.removeEventListener !== 'function')) {
          throw invalid('ERR_INVALID_ARG_TYPE', 'options.signal must be an AbortSignal');
        }
        if (signal?.aborted || !validListener(listener)) return;
        type = `${type}`;
        let root = map.get(type);
        if (!root) {
          root = {next: null, tail: null};
          root.tail = root;
          map.set(type, root);
        }
        for (let entry = root.next; entry; entry = entry.next) {
          if (entry.listener === listener && entry.capture === capture) return;
        }
        const entry = {
          listener, capture, once, passive, signal, abort: null, removed: false,
          resistant: !!options[resistStop], previous: root.tail, next: null,
        };
        root.tail.next = entry;
        root.tail = entry;
        if (signal) {
          entry.abort = () => detach(map, type, root, entry);
          signal.addEventListener('abort', entry.abort,
              {once: true, [resistStop]: true});
        }
      }
      removeEventListener(type, listener, options = {}) {
        const map = stateFor(targetStates, this, 'EventTarget');
        requireArguments(arguments.length, 2);
        if (!validListener(listener)) return;
        type = `${type}`;
        const capture = typeof options === 'boolean' ? options : !!options?.capture;
        const root = map.get(type);
        for (let entry = root?.next; entry; entry = entry.next) {
          if (entry.listener === listener && entry.capture === capture) {
            detach(map, type, root, entry);
            return;
          }
        }
      }
      dispatchEvent(event) {
        const map = stateFor(targetStates, this, 'EventTarget');
        requireArguments(arguments.length, 1);
        const state = eventStates.get(event);
        if (!state) throw invalid('ERR_INVALID_ARG_TYPE', 'event must be an Event');
        if (state.dispatching)
          throw invalid('ERR_EVENT_RECURSION', 'Event is already being dispatched', Error);
        state.target = this;
        state.dispatching = true;
        const type = state.type;
        const root = map.get(type);
        try {
          for (let entry = root?.next; entry;) {
            const next = entry.next;
            if (!entry.removed && (!state.immediate || entry.resistant)) {
              if (entry.once) detach(map, type, root, entry);
              state.passive = entry.passive;
              try {
                const callback = typeof entry.listener === 'function' ?
                    entry.listener : entry.listener.handleEvent;
                if (callback != null) {
                  const receiver = typeof entry.listener === 'function' ? this : entry.listener;
                  const result = Reflect.apply(callback, receiver, [event]);
                  if (result != null) {
                    const then = result.then;
                    if (typeof then === 'function')
                      Reflect.apply(then, result, [undefined, reportError]);
                  }
                }
              } catch (error) {
                reportError(error);
              } finally {
                state.passive = false;
              }
            }
            entry = next;
          }
        } finally {
          state.dispatching = false;
        }
        return !event.defaultPrevented;
      }
    }
    for (const [Constructor, name] of [[Event, 'Event'],
      [CustomEvent, 'CustomEvent'], [EventTarget, 'EventTarget']]) {
      Object.defineProperty(Constructor.prototype, Symbol.toStringTag,
          {value: name, configurable: true});
      for (const key of Object.getOwnPropertyNames(Constructor.prototype)) {
        if (key === 'constructor') continue;
        Object.defineProperty(Constructor.prototype, key, {enumerable: true});
      }
    }
    Object.defineProperty(Event.prototype, 'isTrusted', {configurable: false});
    for (const [name, value] of Object.entries({
      NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3,
    })) {
      Object.defineProperty(Event, name, {value, enumerable: true});
    }
    return {Event, CustomEvent, EventTarget};
  }

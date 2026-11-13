// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createNotificationClass({EventEmitter, nativeImage, isReady, callHost,
    createEvent = sender => ({sender}), host = globalThis}) {
  const states = new WeakMap(), active = new Map();
  const defaults = {title: '', body: '', silent: false, timeoutType: 'default',
    urgency: 'low', subtitle: '', hasReply: false, replyPlaceholder: '', sound: '',
    closeButtonText: '', toastXml: '', actions: []};
  const supported = new Set(['title', 'body', 'silent', 'timeoutType', 'urgency']);
  const fail = (code, message, Type = Error) => Object.assign(new Type(message), {code});
  const invalid = message => fail('ERR_INVALID_ARG_TYPE', message, TypeError);
  let nextId = 0, stopped = false;
  const imageToPNG = Object.getPrototypeOf(nativeImage.createEmpty()).toPNG;
  function stateOf(notification) {
    const state = states.get(notification);
    if (!state) throw invalid('Expected a Notification');
    return state;
  }
  function option(name, value) {
    if (name === 'actions') {
      if (!Array.isArray(value)) throw invalid('Notification.actions must be an array');
      if (value.length) throw fail('ERR_NOT_SUPPORTED', 'Notification.actions is not supported');
      return [];
    }
    if (typeof value !== typeof defaults[name])
      throw invalid('Notification.' + name + ' must be a ' + typeof defaults[name]);
    if (!supported.has(name) && value !== defaults[name])
      throw fail('ERR_NOT_SUPPORTED', 'Notification.' + name + ' is not supported');
    if ((name === 'timeoutType' && !['default', 'never'].includes(value)) ||
        (name === 'urgency' && !['low', 'normal', 'critical'].includes(value)))
      throw fail('ERR_INVALID_ARG_VALUE', 'Invalid Notification.' + name, TypeError);
    return value;
  }
  function icon(value) {
    if (value === undefined) return '';
    const image = typeof value === 'string' ? nativeImage.createFromPath(value) : value;
    // Invoke the trusted NativeImage implementation to preserve its private
    // brand check. An arbitrary object's toPNG method cannot forge an image.
    const bytes = imageToPNG.call(image);
    return bytes.length ? bytes.toString('base64') : '';
  }
  class Notification extends EventEmitter {
    constructor(options = {}) {
      super();
      if (!isReady()) throw fail('ERR_NOT_READY', 'Cannot create Notification before app is ready');
      if (!options || typeof options !== 'object' || Array.isArray(options))
        throw invalid('Notification options must be an object');
      const values = {...defaults, actions: []};
      // Electron reads its known dictionary members. Unrelated properties are
      // ignored, including getters that belong to application metadata.
      for (const name of Object.keys(defaults)) {
        const value = options[name];
        if (value !== undefined) values[name] = option(name, value);
      }
      states.set(this, {values, iconPng: icon(options.icon), id: null});
    }
    static isSupported() { return callHost({method: 'isSupported'}) === true; }
    show() {
      const state = stateOf(this);
      if (stopped) throw fail('ERR_INVALID_STATE', 'Notification runtime has stopped');
      this.close();
      const id = ++nextId;
      state.id = id;
      active.set(id, this);
      const options = Object.fromEntries([...supported].map(name => [name, state.values[name]]));
      if (state.iconPng) options.iconPng = state.iconPng;
      try { callHost({method: 'show', id, options}); }
      catch (error) {
        active.delete(id);
        if (state.id === id) state.id = null;
        throw error;
      }
    }
    close() {
      const state = stateOf(this);
      if (state.id === null) return;
      const id = state.id;
      callHost({method: 'close', id});
      active.delete(id);
      if (state.id === id) state.id = null;
    }
  }
  for (const name of Object.keys(defaults)) {
    Object.defineProperty(Notification.prototype, name, {
      enumerable: true, configurable: true,
      get() {
        const value = stateOf(this).values[name];
        return name === 'actions' ? value.slice() : value;
      },
      set(value) { stateOf(this).values[name] = option(name, value); },
    });
  }
  host.__xenonDispatchNotificationEvent = details => {
    if (!details || !Number.isSafeInteger(details.id)) return;
    const notification = active.get(details.id);
    if (!notification) return;
    const state = stateOf(notification);
    if (state.id !== details.id) return;
    const name = details.event;
    if (!['show', 'click', 'close', 'failed'].includes(name)) return;
    if (name === 'close' || name === 'failed') {
      active.delete(details.id);
      state.id = null;
    }
    notification.emit(name, createEvent(notification),
      ...(name === 'failed' ? [String(details.args?.[0] || 'Notification failed')] : []));
  };
  host.__xenonShutdownNotifications = () => {
    if (stopped) return;
    stopped = true;
    for (const [id, notification] of active) {
      const state = stateOf(notification);
      state.id = null;
      try { callHost({method: 'close', id}); } catch {}
    }
    active.clear();
  };
  return Notification;
}

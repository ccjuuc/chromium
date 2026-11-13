// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Browser-owned requests select a Session and registration. Functions stay in
// this main isolate; renderer IPC channels cannot dispatch these callbacks.
const webRequestRegistrations = new Map();
const pendingWebRequests = new Map();
const webRequestBlockingEvents = new Set([
  'onBeforeRequest', 'onBeforeSendHeaders', 'onHeadersReceived',
]);
const webRequestResourceTypes = new Set([
  'mainFrame', 'subFrame', 'stylesheet', 'script', 'image', 'font', 'object',
  'xhr', 'ping', 'cspReport', 'media', 'webSocket',
]);
let nextWebRequestRegistration = 0;

function webRequestError(code, message, Constructor = Error) {
  return Object.assign(new Constructor(message), {code});
}

function webRequestFilter(value) {
  if (value === undefined) return {urls: ['<all_urls>'], excludeUrls: [], types: []};
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw webRequestError('ERR_INVALID_ARG_TYPE', 'WebRequest filter must be an object', TypeError);
  for (const name of Object.keys(value)) {
    if (!['urls', 'excludeUrls', 'types'].includes(name))
      throw webRequestError('ERR_NOT_SUPPORTED', 'Unsupported WebRequest filter: ' + name);
  }
  const result = {};
  for (const name of ['urls', 'excludeUrls', 'types']) {
    const entries = value[name] === undefined && name !== 'urls' ? [] : value[name];
    if (!Array.isArray(entries) || entries.some(entry => typeof entry !== 'string' || entry.includes('\0')))
      throw webRequestError('ERR_INVALID_ARG_TYPE', `WebRequest filter.${name} must be an array of strings`, TypeError);
    if (name === 'types' && entries.some(entry => !webRequestResourceTypes.has(entry)))
      throw webRequestError('ERR_INVALID_ARG_VALUE', 'Invalid WebRequest resource type', TypeError);
    result[name] = [...new Set(entries)];
  }
  // Match Electron 39's empty-urls behavior. The browser validates the actual
  // URLPattern syntax, schemes, ports and capabilities before accepting it.
  if (!result.urls.length) result.urls.push('<all_urls>');
  return result;
}

function createSessionWebRequest(partition) {
  const active = new Map();
  function register(event, args) {
    if (args.length < 1 || args.length > 2)
      throw webRequestError('ERR_INVALID_ARG_TYPE', 'Expected an optional filter and listener', TypeError);
    const listener = args[args.length - 1];
    if (listener !== null && typeof listener !== 'function')
      throw webRequestError('ERR_INVALID_ARG_TYPE', 'WebRequest listener must be a function or null', TypeError);
    const filter = webRequestFilter(args.length === 2 ? args[0] : undefined);
    if (typeof __xenonBrowserWindowCall !== 'function')
      throw webRequestError('ERR_NOT_SUPPORTED', 'WebRequest host is unavailable');
    const registrationId = listener === null ? (active.get(event) || '') : String(++nextWebRequestRegistration);
    if (listener) webRequestRegistrations.set(registrationId, {partition, event, listener, active});
    try {
      __xenonBrowserWindowCall(0, 'web-request-handler', {
        partition, event, registrationId, filter, enabled: listener !== null,
      });
    } catch (error) {
      if (listener) webRequestRegistrations.delete(registrationId);
      throw error;
    }
    if (listener) active.set(event, registrationId);
    else active.delete(event);
    // Retired listeners remain addressable until the browser releases their
    // registration after its queued requests. Already-running callbacks hold
    // their own listener snapshot and can finish after that release.
  }
  const result = {};
  for (const event of webRequestBlockingEvents)
    result[event] = (...args) => register(event, args);
  for (const event of ['onSendHeaders', 'onResponseStarted',
    'onBeforeRedirect', 'onCompleted', 'onErrorOccurred']) {
    result[event] = () => {
      throw webRequestError('ERR_NOT_SUPPORTED', `Electron webRequest.${event} is not supported`);
    };
  }
  return result;
}

function webRequestResponse(event, value) {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw webRequestError('ERR_INVALID_ARG_TYPE', 'WebRequest callback requires a response object', TypeError);
  const result = {};
  if (value.cancel !== undefined) {
    if (typeof value.cancel !== 'boolean')
      throw webRequestError('ERR_INVALID_ARG_TYPE', 'cancel must be a boolean', TypeError);
    result.cancel = value.cancel;
    if (value.cancel) return result;
  }
  const allowed = event === 'onBeforeRequest' ? ['cancel', 'redirectURL'] :
      event === 'onBeforeSendHeaders' ? ['cancel', 'requestHeaders'] :
      ['cancel', 'responseHeaders', 'statusLine'];
  for (const key of Object.keys(value)) {
    if (!allowed.includes(key))
      throw webRequestError('ERR_NOT_SUPPORTED', `Unsupported ${event} response field: ${key}`);
  }
  for (const name of ['redirectURL', 'statusLine']) {
    if (value[name] === undefined) continue;
    if (typeof value[name] !== 'string' || /[\r\n\0]/.test(value[name]))
      throw webRequestError('ERR_INVALID_ARG_VALUE', `${name} must be a string without control separators`, TypeError);
    result[name] = value[name];
  }
  if (value.requestHeaders !== undefined) {
    const headers = value.requestHeaders;
    if (!headers || typeof headers !== 'object' || Array.isArray(headers))
      throw webRequestError('ERR_INVALID_ARG_TYPE', 'requestHeaders must be an object', TypeError);
    result.requestHeaders = Object.create(null);
    for (const [name, headerValue] of Object.entries(headers)) {
      if (!/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(name))
        throw webRequestError('ERR_INVALID_ARG_VALUE', 'Invalid request header name', TypeError);
      if (typeof headerValue !== 'string' || /[\r\n\0]/.test(headerValue))
        throw webRequestError('ERR_INVALID_ARG_VALUE', 'Invalid request header value', TypeError);
      // Preserve JavaScript's last spelling before native dictionary ordering.
      // An empty dictionary replaces all headers; an omitted field keeps them.
      result.requestHeaders[name.toLowerCase()] = headerValue;
    }
  }
  if (value.responseHeaders !== undefined) {
    const headers = value.responseHeaders;
    if (!headers || typeof headers !== 'object' || Array.isArray(headers))
      throw webRequestError('ERR_INVALID_ARG_TYPE', 'responseHeaders must be an object', TypeError);
    result.responseHeaders = Object.create(null);
    for (const [name, values] of Object.entries(headers)) {
      if (!/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(name))
        throw webRequestError('ERR_INVALID_ARG_VALUE', 'Invalid response header name', TypeError);
      const list = typeof values === 'string' ? [values] : values;
      if (!Array.isArray(list) || list.some(item => typeof item !== 'string' || /[\r\n\0]/.test(item)))
        throw webRequestError('ERR_INVALID_ARG_VALUE', 'Invalid response header value', TypeError);
      // Native dictionaries sort keys; normalize while JS insertion order is
      // still available so {...oldHeaders, 'Content-Security-Policy': ...}
      // replaces an earlier lower-case spelling instead of adding a policy.
      result.responseHeaders[name.toLowerCase()] = [...list];
    }
  }
  return result;
}

globalThis.__xenonDispatchWebRequest = async request => {
  if (!request || typeof request !== 'object' ||
      typeof request.requestId !== 'string' || !request.requestId ||
      typeof request.registrationId !== 'string' ||
      !webRequestBlockingEvents.has(request.event) ||
      !request.details || typeof request.details !== 'object' || Array.isArray(request.details))
    throw webRequestError('ERR_INVALID_ARG_VALUE', 'Invalid WebRequest dispatch');
  const {requestId, registrationId, event} = request;
  const registration = webRequestRegistrations.get(registrationId);
  if (!registration || registration.partition !== request.partition || registration.event !== event)
    throw webRequestError('ERR_ACCESS_DENIED', 'WebRequest registration is unavailable in this Session');
  if (pendingWebRequests.has(requestId))
    throw webRequestError('ERR_INVALID_ARG_VALUE', 'Duplicate WebRequest identity');
  if (pendingWebRequests.size >= 128)
    throw webRequestError('ERR_RESOURCE_BUSY', 'Too many pending WebRequest callbacks');
  return new Promise((resolve, reject) => {
    let settled = false;
    let timer;
    const finish = (error, value, failed = true) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      pendingWebRequests.delete(requestId);
      if (failed) reject(error);
      else resolve(value);
    };
    const callback = value => {
      if (settled) return;
      try { finish(null, webRequestResponse(event, value), false); }
      catch (error) { finish(error); }
    };
    pendingWebRequests.set(requestId, finish);
    timer = setTimeout(() => finish(webRequestError('ERR_TIMED_OUT', 'WebRequest callback timed out')), 30000);
    try {
      const returned = registration.listener({...request.details}, callback);
      // A fulfilled async listener still must call callback. Rejection is a
      // failure, rather than an unhandled promise followed by a hung request.
      if (returned && typeof returned.then === 'function')
        Promise.resolve(returned).catch(error => finish(error));
    } catch (error) {
      finish(error);
    }
  });
};

globalThis.__xenonCancelWebRequest = requestId => {
  pendingWebRequests.get(requestId)?.(webRequestError('ABORT_ERR', 'WebRequest was cancelled'));
};

globalThis.__xenonReleaseWebRequestRegistration = registrationId => {
  const registration = webRequestRegistrations.get(registrationId);
  if (registration?.active.get(registration.event) === registrationId)
    registration.active.delete(registration.event);
  webRequestRegistrations.delete(registrationId);
};

globalThis.__xenonShutdownWebRequests = () => {
  for (const finish of pendingWebRequests.values())
    finish(webRequestError('ABORT_ERR', 'WebRequest container stopped'));
  for (const registration of webRequestRegistrations.values()) registration.active.clear();
  webRequestRegistrations.clear();
};

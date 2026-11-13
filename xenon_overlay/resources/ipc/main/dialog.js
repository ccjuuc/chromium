// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function createElectronDialogs({BrowserWindow, callSync, callAsync, isReady, platform}) {
  const invalid = message => Object.assign(new TypeError(message),
      {code: 'ERR_INVALID_ARG_TYPE'});
  const unsupported = name => Object.assign(new Error(`dialog option ${name} is not supported`),
      {code: 'ERR_NOT_SUPPORTED'});
  function argumentsFor(windowOrOptions, maybeOptions) {
    if (!isReady()) throw Object.assign(new Error('dialog requires app readiness'),
        {code: 'ERR_NOT_READY'});
    let owner = null;
    let options = windowOrOptions;
    if (windowOrOptions instanceof BrowserWindow) {
      owner = windowOrOptions;
      options = maybeOptions;
    } else if (maybeOptions !== undefined) {
      if (windowOrOptions != null) throw invalid('dialog owner must be a BrowserWindow');
      options = maybeOptions;
    }
    if (owner?.isDestroyed()) throw Object.assign(new Error('BrowserWindow has been destroyed'),
        {code: 'ERR_OBJECT_DESTROYED'});
    if (options === undefined || options === null) options = {};
    if (typeof options !== 'object' || Array.isArray(options))
      throw invalid('dialog options must be an object');
    return {id: owner ? owner.id : 0, options: {...options}};
  }
  function strings(options, names) {
    for (const name of names) {
      if (options[name] !== undefined && typeof options[name] !== 'string')
        throw invalid(`${name} must be a string`);
    }
  }
  function booleans(options, names) {
    for (const name of names) {
      if (options[name] !== undefined && typeof options[name] !== 'boolean')
        throw invalid(`${name} must be a boolean`);
    }
  }
  function messageOptions(options) {
    if (options.icon != null) throw unsupported('icon');
    if (options.signal != null) throw unsupported('signal');
    if (options.textWidth !== undefined && options.textWidth !== 0) throw unsupported('textWidth');
    delete options.icon;
    delete options.signal;
    delete options.textWidth;
    strings(options, ['type', 'title', 'message', 'detail', 'checkboxLabel']);
    booleans(options, ['normalizeAccessKeys', 'noLink']);
    if (options.checkboxChecked && !options.checkboxLabel)
      throw invalid('checkboxLabel is required when checkboxChecked is true');
    const type = options.type === undefined ? 'none' : options.type;
    if (!['none', 'info', 'error', 'question', 'warning'].includes(type))
      throw Object.assign(new TypeError('Invalid message box type'), {code: 'ERR_INVALID_ARG_VALUE'});
    let buttons = options.buttons === undefined ? [] : options.buttons;
    if (!Array.isArray(buttons) || buttons.some(value => typeof value !== 'string'))
      throw invalid('buttons must be an array of strings');
    buttons = buttons.length ? buttons.slice() : ['OK'];
    const {defaultId = -1} = options;
    let cancelId = options.cancelId ?? -1;
    for (const [name, value] of [['defaultId', defaultId], ['cancelId', cancelId]]) {
      if (!Number.isInteger(value) || value < -1 || value >= buttons.length)
        throw Object.assign(new RangeError(`${name} must be a button index or -1`),
            {code: 'ERR_OUT_OF_RANGE'});
    }
    if (options.cancelId == null) {
      cancelId = defaultId === 0 && buttons.length > 1 ? 1 : 0;
      const labels = buttons.map(label => {
        if (!options.normalizeAccessKeys) return label;
        if (platform === 'darwin') return label.replace(/&(&?)/g, '$1');
        if (platform === 'linux') return label.replace(/_/g, '__')
            .replace(/&(.?)/g, (_, next) => next === '&' ? '&' : '_' + next);
        return label;
      });
      const index = labels.findIndex(label => /^(cancel|no)$/i.test(label));
      if (index !== -1) cancelId = index;
    }
    return {...options, type, buttons, defaultId, cancelId,
      title: options.title ?? '', message: options.message ?? '', detail: options.detail ?? '',
      checkboxLabel: options.checkboxLabel ?? '', checkboxChecked: !!options.checkboxChecked,
      normalizeAccessKeys: options.normalizeAccessKeys ?? false, noLink: options.noLink ?? false};
  }
  function fileOptions(options) {
    strings(options, ['title', 'defaultPath', 'buttonLabel', 'message', 'nameFieldLabel']);
    booleans(options, ['showsTagField', 'securityScopedBookmarks']);
    if (options.defaultPath?.includes('\0')) throw invalid('defaultPath must not contain NUL');
    if (options.filters !== undefined) {
      if (!Array.isArray(options.filters)) throw invalid('filters must be an array');
      options.filters = options.filters.map(filter => {
        if (!filter || typeof filter !== 'object' || typeof filter.name !== 'string' ||
            !Array.isArray(filter.extensions) || filter.extensions.length === 0 ||
            filter.extensions.some(extension => typeof extension !== 'string' || !extension ||
                /[.\\/\0]/.test(extension)))
          throw invalid('Each filter requires a name and nonempty file extensions without dots or separators');
        return {name: filter.name, extensions: filter.extensions.slice()};
      });
    }
    if (options.properties !== undefined) {
      if (!Array.isArray(options.properties) || options.properties.some(value => typeof value !== 'string'))
        throw invalid('properties must be an array of strings');
      options.properties = options.properties.slice();
    }
    return options;
  }
  function annotateError(error) {
    if (error && !error.code) {
      const match = /^([A-Z][A-Z_]+):/.exec(String(error.message));
      if (match) error.code = match[1];
    }
    return error;
  }
  function invoke(sync, command, normalize, owner, options) {
    const request = argumentsFor(owner, options);
    const details = normalize(request.options);
    if (sync) {
      if (typeof callSync !== 'function') throw unsupported('synchronous native host');
      try { return callSync(request.id, command, details); }
      catch (error) { throw annotateError(error); }
    }
    // Native dialogs resolve only after a real user choice. The async bridge
    // leaves the application event loop running while the dialog is open.
    return Promise.resolve().then(() => {
      if (typeof callAsync !== 'function') throw unsupported('asynchronous native host');
      return callAsync(request.id, command, details);
    })
        .catch(error => { throw annotateError(error); });
  }
  return {
    showMessageBox(owner, options) {
      return invoke(false, 'electron-message-box', messageOptions, owner, options);
    },
    showMessageBoxSync(owner, options) {
      return invoke(true, 'electron-message-box', messageOptions, owner, options).response;
    },
    showSaveDialog(owner, options) {
      return invoke(false, 'electron-save-dialog', fileOptions, owner, options);
    },
    showSaveDialogSync(owner, options) {
      const result = invoke(true, 'electron-save-dialog', fileOptions, owner, options);
      return result.canceled ? undefined : result.filePath;
    },
    showOpenDialog(owner, options) {
      return invoke(false, 'electron-open-dialog', fileOptions, owner, options);
    },
    showOpenDialogSync(owner, options) {
      const result = invoke(true, 'electron-open-dialog', fileOptions, owner, options);
      return result.canceled ? undefined : result.filePaths;
    },
  };
}

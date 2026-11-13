  const sessionPermissionHandlers = new Map();
  let nextPermissionRegistration = 1;

  function callElectronPermissions(request) {
    if (typeof __xenonBrowserWindowCall !== 'function')
      return electronUnsupported('session permission handlers');
    return __xenonBrowserWindowCall(0, 'electron-permissions', request);
  }

  function setSessionPermissionHandler(partition, kind, handler, options) {
    if (handler !== null && typeof handler !== 'function')
      throw new TypeError('Permission handler must be a function or null');
    if (kind === 'display' && options !== undefined) {
      if (!options || typeof options !== 'object' || Array.isArray(options) ||
          (options.useSystemPicker !== undefined && typeof options.useSystemPicker !== 'boolean'))
        throw new TypeError('Invalid display media handler options');
      if (options.useSystemPicker === true)
        return electronUnsupported('session display media system picker');
    }
    if (nextPermissionRegistration > 0x7fffffff)
      throw new RangeError('Permission registration id limit exceeded');
    const registration = nextPermissionRegistration++;
    let handlers = sessionPermissionHandlers.get(partition);
    if (!handlers) sessionPermissionHandlers.set(partition, handlers = new Map());
    const previous = handlers.get(kind);
    const entry = {registration, handler};
    // Browser permission observers can synchronously query the new id while
    // registration is returning through Mojo. Publish the closure first, and
    // restore it if the native side rejects this registration.
    handlers.set(kind, entry);
    try {
      callElectronPermissions({method: 'set-handler', partition, kind,
        registration, enabled: handler !== null});
    } catch (error) {
      if (handlers.get(kind) === entry) {
        if (previous) handlers.set(kind, previous);
        else handlers.delete(kind);
      }
      throw error;
    }
  }

  function permissionContents(id) {
    if (!id) return null;
    const contents = guestWebContents.get(id) || BrowserWindow.fromId(id)?.webContents;
    return contents && !contents.isDestroyed() ? contents : null;
  }

  function permissionHandler(request, kind) {
    const entry = sessionPermissionHandlers.get(request.partition)?.get(kind);
    if (!entry || !entry.handler || entry.registration !== request.registration) {
      const error = new Error('Permission handler registration is no longer active');
      error.code = 'ERR_ABORTED';
      throw error;
    }
    return entry;
  }

  globalThis.__xenonCheckPermission = request => {
    const {handler} = permissionHandler(request, 'check');
    const result = handler(permissionContents(request.webContentsId),
      request.permission, request.requestingOrigin, {...request.details});
    if (typeof result !== 'boolean')
      throw new TypeError('Permission check handler must synchronously return a boolean');
    return result;
  };

  function normalizeDisplaySelection(selection) {
    if (!selection || typeof selection !== 'object' || Array.isArray(selection))
      throw new TypeError('Display media callback requires a streams object');
    const result = {};
    if (selection.video !== undefined) {
      if (!selection.video || typeof selection.video !== 'object')
        throw new TypeError('Display video must be a desktopCapturer source');
      if (typeof selection.video.id !== 'string')
        return electronUnsupported('display media WebFrameMain capture');
      if (!selection.video.id || typeof selection.video.name !== 'string')
        throw new TypeError('Invalid desktopCapturer source');
      result.video = {id: selection.video.id, name: selection.video.name};
    }
    if (selection.audio !== undefined) {
      if (selection.audio !== 'loopback' && selection.audio !== 'loopbackWithMute')
        return electronUnsupported('display media WebFrameMain audio');
      result.audio = selection.audio;
    }
    if (selection.enableLocalEcho !== undefined) {
      if (typeof selection.enableLocalEcho !== 'boolean')
        throw new TypeError('enableLocalEcho must be boolean');
      result.enableLocalEcho = selection.enableLocalEcho;
    }
    return result;
  }

  function dispatchElectronPermissionRequest(request) {
    const kind = request.kind;
    if (kind !== 'request' && kind !== 'display') return;
    let responded = false;
    const respond = result => {
      if (responded) return;
      responded = true;
      const args = {method: 'response', partition: request.partition, kind,
        registration: request.registration, requestId: request.requestId};
      if (kind === 'request') args.granted = result === true;
      else args.selection = result;
      callElectronPermissions(args);
    };
    try {
      const {handler} = permissionHandler(request, kind);
      const contents = permissionContents(request.webContentsId);
      if (kind === 'request') {
        handler(contents, request.permission, allowed => {
          if (typeof allowed !== 'boolean') {
            respond(false);
            throw new TypeError('Permission response must be boolean');
          }
          respond(allowed);
        }, {...request.details});
      } else {
        const details = {...request.details};
        const frame = details.frame;
        if (frame) {
          const endpoints = contents && webContentsState.get(contents)?.endpoints;
          details.frame = Array.from(endpoints?.values() || []).find(endpoint =>
            endpoint.processId === frame.processId && endpoint.frameId === frame.routingId)?.frame ||
            new WebFrameMain({processId: frame.processId, frameId: frame.routingId,
              url: details.requestingUrl});
        }
        handler(details, selection => {
          try { respond(normalizeDisplaySelection(selection)); }
          catch (error) { respond({}); throw error; }
        });
      }
    } catch (error) {
      respond(kind === 'request' ? false : {});
      console.error('Electron permission handler failed:', error);
    }
  }

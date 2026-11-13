  // Electron 44 adds MIME-based ClipboardItems. Keep the older synchronous
  // text/HTML calls intact: this runtime does not claim the v44-only surface.
  function createClipboardApi({callSync, callAsync, Blob: BlobConstructor}) {
    const entries = new WeakMap();
    const supported = new Set(['text/plain', 'text/html', 'text/rtf',
      'image/svg+xml', 'image/png']);
    const invalid = message => Object.assign(new TypeError(message),
        {code: 'ERR_INVALID_ARG_TYPE'});
    const unsupported = mime => Object.assign(
        new Error('Clipboard MIME type is not supported: ' + mime),
        {code: 'ERR_NOT_SUPPORTED'});
    const makeBlob = (bytes, mime) => {
      if (typeof BlobConstructor !== 'function')
        throw Object.assign(new Error('Blob is not available in this runtime'),
            {code: 'ERR_NOT_SUPPORTED'});
      return new BlobConstructor([bytes], {type: mime});
    };
    const stateOf = item => {
      const state = entries.get(item);
      if (!state) throw invalid('Expected an Electron ClipboardItem');
      return state;
    };
    const bytesOf = async value => {
      value = await value;
      if (typeof value === 'string') {
        // Plain V8 main contexts may still use the legacy TextEncoder shim.
        // Blob owns USVString/UTF-8 conversion, including lone surrogates.
        const blob = makeBlob(value, '');
        return Buffer.from(new Uint8Array(await blob.arrayBuffer()));
      }
      if (typeof BlobConstructor === 'function' && value instanceof BlobConstructor)
        return Buffer.from(new Uint8Array(await value.arrayBuffer()));
      throw invalid('ClipboardItem payload values must be a string or a Blob');
    };

    class ClipboardItem {
      constructor(items) {
        if (!items || typeof items !== 'object' || Array.isArray(items))
          throw invalid('ClipboardItem requires an object mapping MIME types to payloads');
        const data = {...items};
        if (Object.keys(data).length === 0)
          throw invalid('ClipboardItem requires at least one MIME type');
        entries.set(this, {data, types: Object.keys(data), readSide: false});
      }
      get types() { return [...stateOf(this).types]; }
      async getType(mime) {
        const state = stateOf(this);
        if (typeof mime !== 'string') throw invalid('ClipboardItem type must be a string');
        if (!state.types.includes(mime))
          throw new Error("The type '" + mime + "' was not found in the ClipboardItem");
        if (state.readSide) {
          const encoded = await callAsync('clipboard.readType', {type: 'clipboard', mime});
          return makeBlob(Buffer.from(encoded, 'base64'), mime);
        }
        const value = await state.data[mime];
        if (mime === 'electron application/bookmark') {
          if (!value || typeof value !== 'object' || typeof value.title !== 'string' ||
              typeof value.url !== 'string')
            throw invalid('Clipboard bookmark must contain string title and url properties');
          return value;
        }
        if (typeof BlobConstructor === 'function' && value instanceof BlobConstructor) return value;
        if (typeof value === 'string') return makeBlob(value, mime);
        return makeBlob(await bytesOf(value), mime);
      }
    }

    const clipboard = {
      readText: (type = 'clipboard') => callSync('clipboard.readText', {type}),
      writeText(text, type = 'clipboard') { callSync('clipboard.writeText', {text, type}); },
      readHTML: (type = 'clipboard') => callSync('clipboard.readHTML', {type}),
      writeHTML(markup, type = 'clipboard') { callSync('clipboard.writeHTML', {markup, type}); },
      clear(type = 'clipboard') { callSync('clipboard.clear', {type}); },
      async read() {
        const types = await callAsync('clipboard.readTypes', {type: 'clipboard'});
        const item = Object.create(ClipboardItem.prototype);
        entries.set(item, {types: [...types], readSide: true});
        return [item];
      },
      async write(items) {
        if (!Array.isArray(items)) throw invalid('clipboard.write expects an array of ClipboardItem');
        const resolved = [];
        for (const item of items) {
          const state = stateOf(item);
          if (state.readSide)
            throw invalid('clipboard.write cannot accept a ClipboardItem returned from clipboard.read');
          const representations = [];
          for (const mime of state.types) {
            if (!supported.has(mime)) throw unsupported(mime);
            representations.push({mime, data: (await bytesOf(state.data[mime])).toString('base64')});
          }
          resolved.push(representations);
        }
        // Only after every Promise/Blob has resolved and every type is valid
        // may the native side atomically replace the system clipboard.
        await callAsync('clipboard.writeItems', {type: 'clipboard', items: resolved});
      },
      async has(mime) {
        if (typeof mime !== 'string') throw invalid('clipboard.has expects a MIME string');
        return callAsync('clipboard.has', {type: 'clipboard', mime});
      },
    };
    return {clipboard, ClipboardItem};
  }

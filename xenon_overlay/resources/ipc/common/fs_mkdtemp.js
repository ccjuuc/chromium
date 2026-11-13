// Shared fs/original-fs temporary directory contract for main and renderer.
// Native transport performs atomic creation; JavaScript never predicts a name
// or emulates success with an in-memory directory.
function installMkdtemp(fs, {Buffer, path, sync, asynchronous}) {
  const invalid = (message, code = 'ERR_INVALID_ARG_TYPE') =>
    Object.assign(new TypeError(message), {code});
  function prepare(prefix, options) {
    prefix = path(prefix, 'prefix');
    if (options != null && typeof options !== 'string' && typeof options !== 'object')
      throw invalid('options must be a string or object');
    const encoding = (typeof options === 'string' ? options : options?.encoding) || 'utf8';
    if (encoding !== 'buffer' && !Buffer.isEncoding(encoding))
      throw invalid('Invalid mkdtemp encoding', 'ERR_INVALID_ARG_VALUE');
    return {request: {operation:'mkdtemp', path:prefix, returnBytes:true}, encoding};
  }
  function result(bytes, encoding) {
    const buffer = Buffer.from(bytes);
    return encoding === 'buffer' ? buffer : buffer.toString(encoding);
  }
  function failure(error, request) {
    // libuv receives Node's appended six-character template. Preserve that
    // syscall pathname rather than the higher-level unexpanded prefix.
    error.path = request.path + 'XXXXXX';
    error.syscall = 'mkdtemp';
    return error;
  }
  fs.mkdtempSync = function mkdtempSync(prefix, options) {
    const {request, encoding} = prepare(prefix, options);
    try { return result(sync(request), encoding); }
    catch (error) { throw failure(error, request); }
  };
  fs.mkdtemp = function mkdtemp(prefix, options, callback) {
    if (typeof options === 'function') { callback = options; options = undefined; }
    if (typeof callback !== 'function') throw invalid('callback must be a function');
    const {request, encoding} = prepare(prefix, options);
    Promise.resolve().then(() => asynchronous(request)).then(
      bytes => callback(null, result(bytes, encoding)),
      error => callback(failure(error, request)));
  };
  fs.promises.mkdtemp = async function mkdtemp(prefix, options) {
    const {request, encoding} = prepare(prefix, options);
    try { return result(await asynchronous(request), encoding); }
    catch (error) { throw failure(error, request); }
  };
}

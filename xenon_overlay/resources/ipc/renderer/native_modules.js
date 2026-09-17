  // node-sqlite3 API backed by real SQLite on a document-owned worker sequence.
  // Only SQLite values cross IPC; SQL, transactions and bindings stay native.
  const sqliteErrorNames = [
    'OK', 'ERROR', 'INTERNAL', 'PERM', 'ABORT', 'BUSY', 'LOCKED', 'NOMEM',
    'READONLY', 'INTERRUPT', 'IOERR', 'CORRUPT', 'NOTFOUND', 'FULL', 'CANTOPEN',
    'PROTOCOL', 'EMPTY', 'SCHEMA', 'TOOBIG', 'CONSTRAINT', 'MISMATCH', 'MISUSE',
    'NOLFS', 'AUTH', 'FORMAT', 'RANGE', 'NOTADB',
  ];
  function sqliteError(error) {
    const result = error instanceof Error ? error : new Error(String(error));
    const match = result.message.match(/\bSQLITE_([A-Z]+):/);
    if (match) {
      result.code = 'SQLITE_' + match[1];
      result.errno = sqliteErrorNames.indexOf(match[1]);
    }
    return result;
  }
  function sqliteEncode(value) {
    if (value === undefined) return {__xenon_sqlite_undefined__: true};
    if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer) {
      return {__xenon_sqlite_blob__: Buffer.from(value).toString('base64')};
    }
    if (value instanceof Date) return value.getTime();
    if (value instanceof RegExp) return String(value);
    if (value == null) return null;
    if (['string', 'number', 'boolean'].includes(typeof value)) return value;
    throw new TypeError('Unsupported SQLite parameter type');
  }
  function sqliteArguments(args) {
    const values = args.slice();
    const callback = typeof values[values.length - 1] === 'function' ?
        values.pop() : undefined;
    let params;
    if (values.length) {
      const first = values[0];
      if (Array.isArray(first)) {
        params = first.map(sqliteEncode);
      } else if (first && typeof first === 'object' &&
                 !ArrayBuffer.isView(first) && !(first instanceof ArrayBuffer) &&
                 !(first instanceof Date) && !(first instanceof RegExp)) {
        params = Object.fromEntries(
            Object.entries(first).map(([key, value]) => [key, sqliteEncode(value)]));
      } else {
        params = values.map(sqliteEncode);
      }
    }
    return {callback, params};
  }
  function sqliteDecodeRow(row) {
    if (row == null) return undefined;
    return Object.fromEntries(Object.entries(row).map(([key, value]) => [
      key, value && typeof value === 'object' &&
          typeof value.__xenon_sqlite_blob__ === 'string' ?
          Buffer.from(value.__xenon_sqlite_blob__, 'base64') : value,
    ]));
  }
  const sqlitePreparationFailed = Symbol('sqlitePreparationFailed');

  class SqliteStatement extends EventEmitter {
    constructor(db, sql, cb) {
      super();
      this.db = db;
      this.sql = String(sql);
      this._id = 0;
      this._prepareError = null;
      db._schedule(this, cb, async () => {
        try {
          this._id = await db._call('prepare', {sql: this.sql});
        } catch (error) {
          this._prepareError = error;
          throw error;
        }
      });
    }
    _operation(operation, args, complete) {
      const {callback, params} = sqliteArguments(args);
      this.db._schedule(this, callback, () => {
        // node-sqlite3 reports preparation failure once, then discards the
        // statement's queued operations without calling their callbacks.
        if (this._prepareError) return sqlitePreparationFailed;
        return this.db._call(operation, {statementId: this._id, params});
      }, complete);
      return this;
    }
    bind(...args) { return this._operation('bind', args); }
    reset(cb) { return this._operation('reset', typeof cb === 'function' ? [cb] : []); }
    finalize(cb) { return this._operation('finalize', typeof cb === 'function' ? [cb] : []); }
    run(...args) {
      return this._operation('run', args, (info, cb) => {
        this.lastID = info.lastID;
        this.changes = info.changes;
        if (cb) cb.call(this, null);
      });
    }
    get(...args) {
      return this._operation('get', args, (row, cb) => {
        if (cb) cb.call(this, null, sqliteDecodeRow(row));
      });
    }
    all(...args) {
      return this._operation('all', args, (rows, cb) => {
        if (cb) cb.call(this, null, rows.map(sqliteDecodeRow));
      });
    }
    each(...args) {
      const complete = args.length > 1 &&
          typeof args[args.length - 1] === 'function' &&
          typeof args[args.length - 2] === 'function' ? args.pop() : undefined;
      const callback = typeof args[args.length - 1] === 'function' ?
          args.pop() : undefined;
      return this.all(...args, function(error, rows) {
        if (error) {
          if (callback) callback.call(this, error);
          else this.emit('error', error);
          return;
        }
        for (const row of rows) {
          if (callback) callback.call(this, null, row);
        }
        if (complete) complete.call(this, null, rows.length);
      });
    }
  }

  class SqliteDatabase extends EventEmitter {
    constructor(filename, modeOrCb, cb) {
      super();
      this.filename = String(filename);
      this.open = false;
      this._id = 0;
      this._queue = Promise.resolve();
      this._openError = null;
      const callback = typeof modeOrCb === 'function' ? modeOrCb : cb;
      const mode = typeof modeOrCb === 'number' ? modeOrCb : 0x10006;
      this._schedule(this, callback, async () => {
        try {
          this._id = await transport.invoke('__xenon:sqlite', {
            operation: 'open',
            path: this.filename === ':memory:' || !this.filename ?
                this.filename : normalizeFsPath(this.filename),
            mode,
          });
          this.open = true;
          this.emit('open');
        } catch (error) {
          this._openError = error;
          throw error;
        }
      });
    }
    _call(operation, options = {}) {
      if (this._openError) throw this._openError;
      return transport.invoke('__xenon:sqlite', {
        operation, databaseId: this._id, ...options,
      });
    }
    _schedule(owner, cb, operation, complete) {
      const job = this._queue.then(operation);
      this._queue = job.then(value => {
        if (value === sqlitePreparationFailed) return;
        if (complete) complete(value, cb);
        else if (cb) cb.call(owner, null);
      }, error => {
        const failure = sqliteError(error);
        if (cb) cb.call(owner, failure);
        else owner.emit('error', failure);
      }).catch(error => {
        // A caller's callback exception must not stall later database jobs.
        queueMicrotask(() => { throw error; });
      });
      return this;
    }
    _statement(sql, args) {
      const cb = args[args.length - 1];
      return new SqliteStatement(this, sql, typeof cb === 'function' ?
          function(error) { if (error) cb.call(this, error); } : undefined);
    }
    prepare(sql, ...args) {
      const stmt = this._statement(sql, args);
      return args.length ? stmt.bind(...args) : stmt;
    }
    run(sql, ...args) {
      this._statement(sql, args).run(...args).finalize();
      return this;
    }
    get(sql, ...args) {
      this._statement(sql, args).get(...args).finalize();
      return this;
    }
    all(sql, ...args) {
      this._statement(sql, args).all(...args).finalize();
      return this;
    }
    each(sql, ...args) {
      this._statement(sql, args).each(...args).finalize();
      return this;
    }
    exec(sql, cb) {
      return this._schedule(this, cb, () => this._call('exec', {sql: String(sql)}));
    }
    close(cb) {
      return this._schedule(this, cb, async () => {
        await this._call('close');
        this.open = false;
        this.emit('close');
      });
    }
    wait(cb) { return this._schedule(this, cb, () => {}); }
    serialize(cb) { if (cb) cb(); return this; }
    parallelize(cb) { if (cb) cb(); return this; }
    configure(option, value) {
      if (option !== 'busyTimeout' || !Number.isInteger(value) || value < 0) {
        throw new TypeError('Unsupported SQLite configuration');
      }
      return this._schedule(this, undefined,
          () => this._call('configure', {busyTimeout: value}));
    }
  }

  // sqlite3's JS wrapper expects this export even when backup is unused.
  // Unsupported operations report errors, never synthetic successful writes.
  class SqliteBackup extends EventEmitter {
    constructor(...args) {
      super();
      this._fail(args[args.length - 1]);
    }
    _fail(cb) {
      queueMicrotask(() => {
        const error = sqliteError(new Error('SQLITE_MISUSE: Backup is not supported'));
        if (typeof cb === 'function') cb.call(this, error);
        else this.emit('error', error);
      });
      return this;
    }
    step(_pages, cb) { return this._fail(cb); }
    finish(cb) { return this._fail(cb); }
  }

  const sqlite3Module = {
    Database: SqliteDatabase,
    Statement: SqliteStatement,
    Backup: SqliteBackup,
    ...Object.fromEntries(sqliteErrorNames.map((name, index) => [name, index])),
    OPEN_READONLY: 1,
    OPEN_READWRITE: 2,
    OPEN_CREATE: 4,
    OPEN_FULLMUTEX: 0x00010000,
    OPEN_URI: 0x00000040,
    OPEN_SHAREDCACHE: 0x00020000,
    OPEN_PRIVATECACHE: 0x00040000,
    verbose: () => sqlite3Module,
  };

  function loadNativeNodeModule(normalizedPath) {
    const exportsList = transport.requireNodeModuleSync(normalizedPath);
    // Current hosts return the exact root in the load reply. Avoid building,
    // copying and discarding a legacy export tree plus a second sync IPC.
    if (exportsList && !Array.isArray(exportsList) &&
        typeof exportsList.kind === 'string') {
      return buildExactNativeExport(normalizedPath, exportsList);
    }
    if (typeof transport.inspectNodeExportSync === 'function') {
      return buildExactNativeExport(normalizedPath,
          transport.inspectNodeExportSync(normalizedPath, ''));
    }
    // Older transports expose only a shallow tree. Current hosts provide the
    // root descriptor above, including its actual type and lazy nested shape.
    if (!Array.isArray(exportsList)) {
      const error = new Error('Native module load returned an invalid export descriptor');
      error.code = 'ERR_INVALID_NATIVE_EXPORTS';
      throw error;
    }
    const target = {};
    for (const item of exportsList) {
      Object.defineProperty(target, item.name, {
        value: buildExportFromMojoInfo(normalizedPath, item),
        enumerable: true, configurable: true, writable: true,
      });
    }
    return target;
  }

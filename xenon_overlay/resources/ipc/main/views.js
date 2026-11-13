  function createElectronViews({EventEmitter, callNative, createContents, preparePreferences, isReady,
    getContentsNativeId, getWindowId}) {
    const states = new WeakMap();
    const roots = new WeakMap();
    const byId = new Map();
    const rootToken = {};
    const finalizer = typeof FinalizationRegistry === 'function' ?
        new FinalizationRegistry(id => {
          byId.delete(id);
          try { callNative({action: 'release', id}); } catch (_) {
            // Application teardown already releases all native views owned by
            // that service generation; a late GC callback has nothing to free.
          }
        }) : null;

    function state(view) {
      const result = states.get(view);
      if (!result) throw new TypeError('Expected a View');
      return result;
    }
    function native(view, action, values = {}) {
      return callNative({action, id: state(view).id, ...values});
    }
    function register(view, result) {
      if (!result || !Number.isInteger(result.id) || result.id <= 0)
        throw new Error('Invalid native View identity');
      states.set(view, {id: result.id, children: [], parent: null});
      byId.set(result.id, new WeakRef(view));
      finalizer?.register(view, result.id);
    }
    function rect(value) {
      if (!value || typeof value !== 'object') throw new TypeError('bounds must be a Rectangle');
      const result = {};
      for (const key of ['x', 'y', 'width', 'height']) {
        if (!Number.isInteger(value[key]) || value[key] < -2147483648 || value[key] > 2147483647)
          throw new TypeError('bounds.' + key + ' must be an integer');
        result[key] = value[key];
      }
      if (result.width < 0 || result.height < 0) throw new RangeError('View size must not be negative');
      return result;
    }
    function color(value) {
      if (value !== undefined && value !== null && typeof value !== 'string')
        throw new TypeError('color must be a string or null');
      return value ?? null;
    }

    class View extends EventEmitter {
      constructor(options, internal) {
        super();
        if (internal === rootToken) register(this, options);
        else {
          if (options !== undefined && (!options || typeof options !== 'object'))
            throw new TypeError('options must be an object');
          register(this, callNative({action: 'create', kind: 'view'}));
        }
      }
      get children() { return state(this).children.slice(); }
      addChildView(child, index) {
        const parentState = state(this), childState = state(child);
        if (child === this) throw new Error('A View cannot contain itself');
        for (let cursor = this; cursor; cursor = state(cursor).parent?.deref()) {
          if (cursor === child) throw new Error('A View cannot contain one of its ancestors');
        }
        if (index !== undefined && (!Number.isInteger(index) || index < 0))
          throw new TypeError('index must be a non-negative integer');
        native(this, 'add', {childId: childState.id, ...(index === undefined ? {} : {index})});
        const previous = childState.parent?.deref();
        if (previous) {
          const siblings = state(previous).children;
          const at = siblings.indexOf(child);
          if (at >= 0) siblings.splice(at, 1);
        }
        parentState.children.splice(Math.min(index ?? parentState.children.length,
            parentState.children.length), 0, child);
        childState.parent = new WeakRef(this);
      }
      removeChildView(child) {
        const parentState = state(this), childState = state(child);
        if (childState.parent?.deref() !== this) return;
        native(this, 'remove', {childId: childState.id});
        parentState.children.splice(parentState.children.indexOf(child), 1);
        childState.parent = null;
      }
      setBounds(bounds) { native(this, 'set-bounds', {bounds: rect(bounds)}); }
      getBounds() { return native(this, 'get-bounds'); }
      setVisible(visible) {
        if (typeof visible !== 'boolean') throw new TypeError('visible must be a boolean');
        native(this, 'set-visible', {visible});
      }
      getVisible() { return native(this, 'get-visible'); }
      setBackgroundColor(value) { native(this, 'set-background', {color: color(value)}); }
      setLayout() {
        throw Object.assign(new Error('View.setLayout is not supported'), {code: 'ERR_NOT_SUPPORTED'});
      }
    }

    class WebContentsView extends View {
      constructor(options = {}) {
        if (!options || typeof options !== 'object') throw new TypeError('options must be an object');
        if (!isReady()) throw new Error('Cannot create WebContentsView before app is ready');
        const providedPreferences = options.webPreferences ?? {};
        if (!providedPreferences || typeof providedPreferences !== 'object')
          throw new TypeError('options.webPreferences must be an object');
        const preferences = preparePreferences(providedPreferences);
        const existingId = options.webContents === undefined ? undefined :
            getContentsNativeId(options.webContents);
        const result = callNative({action: 'create', kind: 'web-contents',
          preferences, ...(existingId === undefined ? {} : {webContentsId: existingId})});
        super(result, rootToken);
        const contents = options.webContents ?? createContents(result.webContentsId, preferences);
        Object.defineProperty(this, 'webContents', {enumerable: true, value: contents});
      }
      setBorderRadius(radius) {
        if (!Number.isFinite(radius) || radius < 0)
          throw new TypeError('radius must be a non-negative finite number');
        native(this, 'set-border-radius', {radius});
      }
    }

    function contentView(window) {
      if (window.isDestroyed()) throw new Error('Object has been destroyed');
      let view = roots.get(window);
      if (!view) {
        const result = callNative({action: 'root', windowId: getWindowId(window)});
        view = new View(result, rootToken);
        roots.set(window, view);
      }
      return view;
    }
    function dispatch(details) {
      const view = byId.get(details.id)?.deref();
      if (view) view.emit(details.event);
    }
    return {View, WebContentsView, contentView, dispatch};
  }

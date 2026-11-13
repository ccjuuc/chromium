  // MenuItem holds application state. The existing native popup receives a
  // snapshot only when opened, and routes selection back to this same object.
  function createMenuItemClass({Menu, runRole, unsupported, platform}) {
    let nextCommandId = 0;
    const checked = new WeakMap();
    const labels = {
      undo: 'Undo', redo: 'Redo', cut: 'Cut', copy: 'Copy', paste: 'Paste',
      pasteandmatchstyle: 'Paste and Match Style', delete: 'Delete',
      selectall: 'Select All', reload: 'Reload', forcereload: 'Force Reload',
      toggledevtools: 'Toggle Developer Tools', resetzoom: 'Actual Size',
      zoomin: 'Zoom In', zoomout: 'Zoom Out', togglefullscreen: 'Toggle Full Screen',
      minimize: 'Minimize', zoom: 'Zoom', close: 'Close Window', quit: 'Quit',
      filemenu: 'File', editmenu: 'Edit', viewmenu: 'View', windowmenu: 'Window',
    };
    const submenus = {
      filemenu: [{role: platform === 'darwin' ? 'close' : 'quit'}],
      editmenu: [{role: 'undo'}, {role: 'redo'}, {type: 'separator'},
        {role: 'cut'}, {role: 'copy'}, {role: 'paste'}, {role: 'delete'},
        {type: 'separator'}, {role: 'selectall'}],
      viewmenu: [{role: 'reload'}, {role: 'forcereload'}, {role: 'toggledevtools'},
        {type: 'separator'}, {role: 'resetzoom'}, {role: 'zoomin'}, {role: 'zoomout'},
        {type: 'separator'}, {role: 'togglefullscreen'}],
      windowmenu: [{role: 'minimize'}, {role: 'zoom'}, {role: 'close'}],
    };
    const types = ['normal', 'separator', 'submenu', 'checkbox', 'radio'];
    return class MenuItem {
      constructor(options = {}) {
        if (!options || typeof options !== 'object' || Array.isArray(options))
          throw new TypeError('MenuItem options must be an object');
        for (const key of Object.keys(options)) {
          if (!(key in this)) this[key] = options[key];
        }
        const role = this.role == null ? null : String(this.role).toLowerCase();
        let submenu = this.submenu ?? submenus[role];
        if (submenu != null && !(submenu instanceof Menu)) {
          if (!Array.isArray(submenu)) throw new TypeError('Invalid submenu');
          submenu = Menu.buildFromTemplate(submenu);
        }
        const type = this.type ?? (submenu ? 'submenu' : 'normal');
        if (!types.includes(type)) {
          if (type === 'header' || type === 'palette') return unsupported('MenuItem.' + type);
          throw new TypeError('Unknown menu item type: ' + type);
        }
        if (type === 'submenu' && !submenu) throw new TypeError('Invalid submenu');
        for (const [name, value] of Object.entries({type, role,
          submenu: submenu ?? null, accelerator: this.accelerator ?? null,
          icon: this.icon ?? null, commandId: ++nextCommandId})) {
          Object.defineProperty(this, name, {value, enumerable: true, writable: false});
        }
        for (const [name, value] of Object.entries({label: labels[role] ?? '',
          sublabel: '', toolTip: '', enabled: true, visible: true, checked: false,
          acceleratorWorksWhenHidden: true, registerAccelerator: true})) {
          if (this[name] == null) this[name] = value;
        }
        checked.set(this, Boolean(this.checked));
        Object.defineProperty(this, 'checked', {enumerable: true,
          get: () => checked.get(this),
          set: value => {
            if (this.type !== 'radio' || !this.menu) {
              checked.set(this, Boolean(value));
              return;
            }
            // Electron's inserted radio item setter always selects the item,
            // even when assigned false; a group cannot be cleared this way.
            checked.set(this, true);
            const items = this.menu.items;
            const index = items.indexOf(this);
            for (let at = index - 1; at >= 0 && items[at].type !== 'separator'; at--)
              if (items[at].type === 'radio') checked.set(items[at], false);
            for (let at = index + 1; at < items.length && items[at].type !== 'separator'; at++)
              if (items[at].type === 'radio') checked.set(items[at], false);
          },
        });
        Object.defineProperty(this, 'userAccelerator', {
          enumerable: true, get: () => null,
        });
        const handler = options.click;
        this.click = (event, focusedWindow, focusedWebContents) => {
          if (this.type === 'checkbox') this.checked = !this.checked;
          if (this.type === 'radio') this.checked = true;
          if (role && runRole(role, focusedWindow, focusedWebContents)) return;
          if (typeof handler === 'function') handler(this, focusedWindow, event);
        };
      }
      getCheckStatus() { return Boolean(this.checked); }
    };
  }

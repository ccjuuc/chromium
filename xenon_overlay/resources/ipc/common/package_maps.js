  // Package maps are resolved separately from filesystem lookup. In particular,
  // a valid target that is absent on disk must not fall back to package.main or
  // to a later array item. Both main and renderer use this ordered JSON logic.
  function resolvePackageMap(packageJson, key, conditions, isImports = false) {
    const field = isImports ? 'imports' : 'exports';
    const own = (object, name) => Object.prototype.hasOwnProperty.call(object, name);
    const fail = (code, message) => {
      throw Object.assign(new Error(message), {code});
    };
    const missing = () => fail(isImports ? 'ERR_PACKAGE_IMPORT_NOT_DEFINED' :
        'ERR_PACKAGE_PATH_NOT_EXPORTED',
        isImports ? `Package import ${key} is not defined` :
          `Package subpath ${key} is not exported`);
    const invalidTarget = target => fail('ERR_INVALID_PACKAGE_TARGET',
        `Invalid package ${field} target ${JSON.stringify(target)} for ${key}`);
    if (!packageJson || typeof packageJson !== 'object' || Array.isArray(packageJson)) {
      fail('ERR_INVALID_PACKAGE_CONFIG', 'Package configuration must be an object');
    }
    if (typeof key !== 'string' || !Array.isArray(conditions)) {
      fail('ERR_INVALID_ARG_TYPE', 'Package map key and conditions have invalid types');
    }
    const active = new Set(conditions);
    if (isImports && (key === '#' || key.endsWith('/'))) {
      fail('ERR_INVALID_MODULE_SPECIFIER', `Invalid package import specifier ${key}`);
    }
    let map = packageJson[field];
    if (!isImports) {
      let shorthand = typeof map === 'string' || Array.isArray(map);
      if (map && typeof map === 'object' && !Array.isArray(map)) {
        const keys = Object.keys(map);
        const subpaths = keys.filter(name => name.startsWith('.')).length;
        if (subpaths && subpaths !== keys.length) {
          fail('ERR_INVALID_PACKAGE_CONFIG',
              'Package exports cannot mix subpath and condition keys');
        }
        shorthand = keys.length > 0 && !subpaths;
      }
      if (shorthand) map = {'.': map};
    }
    if (!map || typeof map !== 'object' || Array.isArray(map)) return missing();

    const invalidSegments = value => value.split(/[\\/]/).some(segment => {
      let decoded;
      try { decoded = decodeURIComponent(segment); }
      catch (_error) {
        fail('ERR_INVALID_MODULE_SPECIFIER', `Invalid encoded package path ${value}`);
      }
      return decoded === '.' || decoded === '..' || decoded.toLowerCase() === 'node_modules';
    });
    const invalidEncodedSeparator = value => /%(?:2f|5c)/i.test(value);
    const resolveTarget = (target, substitution, pattern) => {
      if (typeof target === 'string') {
        if (!target.startsWith('./')) {
          // imports can redirect to another package, but never to an absolute
          // path or URL (including node: URLs), matching Node package maps.
          if (!isImports || target.startsWith('../') || target.startsWith('/') ||
              /^[A-Za-z][A-Za-z0-9+.-]*:/.test(target)) return invalidTarget(target);
          return pattern ? target.replace(/\*/g, () => substitution) : target;
        }
        if (invalidSegments(target.slice(2))) return invalidTarget(target);
        if (pattern && invalidSegments(substitution)) {
          fail('ERR_INVALID_MODULE_SPECIFIER', `Invalid package subpath ${key}`);
        }
        const result = pattern ? target.replace(/\*/g, () => substitution) : target;
        if (invalidEncodedSeparator(result) || result.includes('\0')) {
          fail('ERR_INVALID_MODULE_SPECIFIER', `Invalid encoded package path ${result}`);
        }
        return result;
      }
      if (Array.isArray(target)) {
        if (!target.length) return null;
        let lastError;
        for (const item of target) {
          let result;
          try { result = resolveTarget(item, substitution, pattern); }
          catch (error) {
            if (error.code !== 'ERR_INVALID_PACKAGE_TARGET') throw error;
            lastError = error;
            continue;
          }
          if (result === undefined) continue;
          if (result === null) { lastError = null; continue; }
          return result;
        }
        if (lastError) throw lastError;
        return lastError;
      }
      if (target && typeof target === 'object') {
        const names = Object.keys(target);
        if (names.some(name => String(Number(name)) === name &&
            Number(name) >= 0 && Number(name) < 0xFFFFFFFF)) {
          fail('ERR_INVALID_PACKAGE_CONFIG', 'Package conditions cannot have numeric keys');
        }
        for (const name of names) {
          if (name !== 'default' && !active.has(name)) continue;
          const result = resolveTarget(target[name], substitution, pattern);
          if (result !== undefined) return result;
        }
        return undefined;
      }
      if (target === null) return null;
      return invalidTarget(target);
    };
    let target;
    let substitution = '';
    let pattern = false;
    if (own(map, key) && !key.includes('*') && !key.endsWith('/')) {
      target = map[key];
    } else {
      let best = '';
      let bestPrefix = -1;
      for (const candidate of Object.keys(map)) {
        const star = candidate.indexOf('*');
        if (star < 0 || star !== candidate.lastIndexOf('*')) continue;
        const prefix = candidate.slice(0, star);
        const suffix = candidate.slice(star + 1);
        if (!key.startsWith(prefix) || !key.endsWith(suffix) || key.length < candidate.length) continue;
        if (star > bestPrefix || (star === bestPrefix && candidate.length > best.length)) {
          best = candidate;
          bestPrefix = star;
        }
      }
      if (!best) return missing();
      substitution = key.slice(bestPrefix, key.length - (best.length - bestPrefix - 1));
      target = map[best];
      pattern = true;
    }
    const result = resolveTarget(target, substitution, pattern);
    return result == null ? missing() : result;
  }

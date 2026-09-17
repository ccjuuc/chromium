  globalThis.global = globalThis;

  const transport = globalThis.xenonIpcRenderer;
  if (!transport || globalThis.__xenonElectronIpc) {
    return;
  }

  // A renderer process may host multiple Electron containers. Query metadata
  // from the current Document instead of using the process-wide fallback.
  const injectedPaths = Object.assign({}, globalThis.__xenonPaths || {});
  try {
    Object.assign(injectedPaths, transport.getRuntimeConfig());
  } catch (error) {
    console.warn('[xenon-ipc] runtime config unavailable:', error);
  }
  const runtimePlatform = injectedPaths.platform;
  const runtimeArch = injectedPaths.arch;
  if (typeof runtimePlatform !== 'string' || !runtimePlatform ||
      typeof runtimeArch !== 'string' || !runtimeArch) {
    throw Object.assign(new Error('Native platform and architecture metadata are unavailable'),
                        {code: 'ERR_NOT_SUPPORTED'});
  }
  const rawExecPath =
      String(injectedPaths.execPath || '');
  // Main and renderer receive the same validated executable identity. An app
  // display name is not a filesystem path and must never rename it here.
  const executableName = rawExecPath.split(/[\\/]/).pop().replace(/\.exe$/i, '');
  const hostedAppName = String(injectedPaths.appName || executableName || 'xenon');
  const hostedAppVersion = String(injectedPaths.appVersion || '1.0.0');
  const execPath = rawExecPath;
  const exeDir = injectedPaths.exeDir ||
      rawExecPath.replace(/[\\/][^\\/]+$/, '');
  const appPath = injectedPaths.appPath || exeDir;
  const userData = injectedPaths.userData || '';
  const appData = injectedPaths.appData || '';
  const localAppData = injectedPaths.localAppData || '';
  const homeDir = injectedPaths.home || '';
  const tempDir = injectedPaths.temp || '';

  const runtimeSeparator = runtimePlatform === 'win32' ? '\\' : '/';
  globalThis.__filename = injectedPaths.documentPath || appPath + runtimeSeparator + 'index.js';
  globalThis.__dirname = globalThis.__filename.replace(
      runtimePlatform === 'win32' ? /[\\/][^\\/]*$/ : /\/[^/]*$/, '');
  if (typeof window !== 'undefined') {
    window.global = window;
    window.__filename = globalThis.__filename;
    window.__dirname = globalThis.__dirname;
  }

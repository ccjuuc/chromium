  const childProcessModule = createChildProcessModule(request => {
    if (typeof __xenonChildProcessCall !== 'function') {
      const error = new Error('Native child process transport is unavailable');
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    }
    return __xenonChildProcessCall(request);
  }, handler => ipcMain.on('__xenon:child-process:event', (_event, value) => handler(value)));

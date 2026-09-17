  const childProcessModule = createChildProcessModule(request => {
    const result = transport.sendSync('__xenon:child-process:call', request);
    if (!result || typeof result.ok !== 'boolean') {
      const error = new Error('Native child process transport is unavailable');
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    }
    return result;
  }, handler => ipcRenderer.on('__xenon:child-process:event', (_event, value) => handler(value)));

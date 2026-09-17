  // Expose the module independently of process creation. No process, PID, exit
  // status or successful callback may be fabricated without an OS operation.
  const childProcessModule = (() => {
    const unavailable = operation => {
      const error = new Error(`child_process.${operation} is not supported by this runtime`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    };
    class ChildProcess extends EventEmitter {
      constructor() { super(); unavailable('ChildProcess'); }
      spawn() { return unavailable('ChildProcess.spawn'); }
      kill() { return unavailable('ChildProcess.kill'); }
      send() { return unavailable('ChildProcess.send'); }
      disconnect() { return unavailable('ChildProcess.disconnect'); }
    }
    return {
      ChildProcess,
      _forkChild: () => unavailable('_forkChild'),
      spawn: () => unavailable('spawn'),
      spawnSync: () => unavailable('spawnSync'),
      exec: () => unavailable('exec'),
      execSync: () => unavailable('execSync'),
      execFile: () => unavailable('execFile'),
      execFileSync: () => unavailable('execFileSync'),
      fork: () => unavailable('fork'),
    };
  })();

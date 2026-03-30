// Xenon component extension — popup UI

function log(line) {
  const el = document.getElementById('log');
  if (!el) {
    return;
  }
  el.classList.remove('log-empty');
  const ts = new Date().toISOString().split('T')[1].replace('Z', '');
  el.textContent = `[${ts}] ${line}\n` + el.textContent;
}

function initExtension() {
  const closeBtn = document.getElementById('closeBtn');
  if (closeBtn) {
    closeBtn.addEventListener('click', () => window.close());
  }

  document.getElementById('btnXenonPrivatePing')?.addEventListener('click', () => {
    if (!chrome?.xenonPrivate?.ping) {
      log('ERROR: chrome.xenonPrivate.ping 不存在（检查 manifest permissions: xenonPrivate）');
      return;
    }
    chrome.xenonPrivate.ping((response) => {
      log(`xenonPrivate.ping OK: ${String(response)}`);
    });
  });

  document.getElementById('btnRuntimeId')?.addEventListener('click', () => {
    const id = chrome?.runtime?.id;
    log(`runtime.id: ${id || '(无)'}`);
  });

  document.getElementById('btnPingBackground')?.addEventListener('click', () => {
    if (!chrome?.runtime?.sendMessage) {
      log('ERROR: runtime.sendMessage 不可用');
      return;
    }
    chrome.runtime.sendMessage({action: 'ping'}, (reply) => {
      const err = chrome.runtime.lastError;
      if (err) {
        log(`sendMessage error: ${err.message}`);
        return;
      }
      log(`background 回复: ${JSON.stringify(reply)}`);
    });
  });
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initExtension);
} else {
  initExtension();
}

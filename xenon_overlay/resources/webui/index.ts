import {PageHandler} from './xenon.mojom-webui.js';

const handler = PageHandler.getRemote();

console.log('Xenon WebUI Loaded');

function setStatus(el: HTMLElement | null, working: boolean, ok: boolean, message: string) {
  if (!el) {
    return;
  }
  if (working) {
    el.textContent = '…';
    el.style.color = '#ffd166';
    return;
  }
  el.textContent = message || (ok ? 'OK' : 'Error');
  el.style.color = ok ? '#06d6a0' : '#ef476f';
}

document.addEventListener('DOMContentLoaded', () => {
  const statusText = document.getElementById('status-text');

  document.getElementById('close-btn')?.addEventListener('click', () => {
    console.log('Close requested via Mojo');
    handler.close();
  });

  document.getElementById('test-main-remote')?.addEventListener('click', () => {
    setStatus(statusText, true, false, '');
    handler.pingMainService().then((result) => {
      const {success, message} = result;
      console.log('PingMainService:', success, message);
      setStatus(statusText, false, success, message);
    });
  });

  document.getElementById('test-shared-remote')?.addEventListener('click', () => {
    setStatus(statusText, true, false, '');
    handler.testSharedRemoteDuplicate().then((result) => {
      const {success, message} = result;
      console.log('TestSharedRemoteDuplicate:', success, message);
      setStatus(statusText, false, success, message);
    });
  });

  document.getElementById('test-associated')?.addEventListener('click', () => {
    setStatus(statusText, true, false, '');
    handler.pingAssociatedRemote().then((result) => {
      const {success, message} = result;
      console.log('PingAssociatedRemote:', success, message);
      setStatus(statusText, false, success, message);
    });
  });

  document.getElementById('test-observer')?.addEventListener('click', () => {
    setStatus(statusText, true, false, '');
    handler.testUtilityToBrowserObserver().then((result) => {
      const {success, message} = result;
      console.log('TestUtilityToBrowserObserver:', success, message);
      setStatus(statusText, false, success, message);
    });
  });
});

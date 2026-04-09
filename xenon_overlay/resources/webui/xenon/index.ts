import {PageHandler} from './xenon.mojom-webui.js';

const handler = PageHandler.getRemote();

/** 与 C++ `JSXenonApi` / xenon_page_api.mojom 对齐。 */
interface XenonPageApi {
  readonly name: string;
  ping(): Promise<string>;
  getApiVersion(): Promise<string>;
  echoObject(obj: unknown): Promise<unknown>;
  wrapObjectWithBrowserMeta(obj: unknown): Promise<unknown>;
}

declare global {
  interface Window {
    xenon?: XenonPageApi;
  }
}

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
  const xenonApiStatus = document.getElementById('xenon-api-status');

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

  document.getElementById('test-page-host-ping')?.addEventListener('click', () => {
    setStatus(xenonApiStatus, true, false, '');
    const x = window.xenon;
    if (!x) {
      setStatus(
          xenonApiStatus, false, false,
          '未注入：需为本页 chrome://xenon-overlay/ 且安全上下文');
      return;
    }
    x.ping()
        .then((message) => {
          console.log('window.xenon.ping:', message);
          setStatus(xenonApiStatus, false, true, message);
        })
        .catch((err) => {
          console.error('window.xenon.ping', err);
          setStatus(xenonApiStatus, false, false, String(err));
        });
  });

  document.getElementById('test-page-host-version')?.addEventListener('click', () => {
    setStatus(xenonApiStatus, true, false, '');
    const x = window.xenon;
    if (!x) {
      setStatus(
          xenonApiStatus, false, false,
          '未注入：需为本页 chrome://xenon-overlay/ 且安全上下文');
      return;
    }
    x.getApiVersion()
        .then((version) => {
          const line = `name=${x.name} · apiVersion=${version}`;
          console.log('window.xenon:', line);
          setStatus(xenonApiStatus, false, true, line);
        })
        .catch((err) => {
          console.error('window.xenon.getApiVersion', err);
          setStatus(xenonApiStatus, false, false, String(err));
        });
  });

  document.getElementById('test-page-host-echo-object')?.addEventListener('click', () => {
    setStatus(xenonApiStatus, true, false, '');
    const x = window.xenon;
    if (!x) {
      setStatus(
          xenonApiStatus, false, false,
          '未注入：需为本页 chrome://xenon-overlay/ 且安全上下文');
      return;
    }
    const sample = {hello: 'xenon', n: 42};
    x.echoObject(sample)
        .then((out) => {
          const ok = JSON.stringify(out) === JSON.stringify(sample);
          const line = ok ? `echoObject OK · ${JSON.stringify(out)}` :
                             `echoObject 值不一致 · ${JSON.stringify(out)}`;
          console.log('window.xenon.echoObject', out);
          setStatus(xenonApiStatus, false, ok, line);
        })
        .catch((err) => {
          console.error('window.xenon.echoObject', err);
          setStatus(xenonApiStatus, false, false, String(err));
        });
  });

  document.getElementById('test-page-host-wrap-object')?.addEventListener('click', () => {
    setStatus(xenonApiStatus, true, false, '');
    const x = window.xenon;
    if (!x) {
      setStatus(
          xenonApiStatus, false, false,
          '未注入：需为本页 chrome://xenon-overlay/ 且安全上下文');
      return;
    }
    x.wrapObjectWithBrowserMeta({a: 1})
        .then((out) => {
          console.log('window.xenon.wrapObjectWithBrowserMeta', out);
          const line =
              typeof out === 'object' && out !== null ? JSON.stringify(out) : String(out);
          setStatus(xenonApiStatus, false, true, line);
        })
        .catch((err) => {
          console.error('window.xenon.wrapObjectWithBrowserMeta', err);
          setStatus(xenonApiStatus, false, false, String(err));
        });
  });

  document.getElementById('test-data-mask')?.addEventListener('click', () => {
    setStatus(statusText, true, false, '');
    handler.testDataMask().then((result) => {
      const {success, message} = result;
      console.log('TestDataMask:', success, message);
      setStatus(statusText, false, success, message);
    });
  });

  document.getElementById('open-component-extension')?.addEventListener('click', () => {
    setStatus(statusText, true, false, '');
    handler.openComponentExtensionDialog().then((result) => {
      const {success, message} = result;
      console.log('OpenComponentExtensionDialog:', success, message);
      setStatus(statusText, false, success, message);
    });
  });
});

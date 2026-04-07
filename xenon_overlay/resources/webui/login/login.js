'use strict';

// chrome://xenon-login/ — 无 Mojo TS；通过 chrome.send 与 C++ XenonLoginWebUIMessageHandler 通信。

document.addEventListener('DOMContentLoaded', () => {
  document.getElementById('login-done-btn')?.addEventListener('click', () => {
    chrome.send('xenonLoginDone', []);
  });
  document.getElementById('logout-test-btn')?.addEventListener('click', () => {
    chrome.send('xenonLoginLogoutTest', []);
  });
  document.getElementById('login-close-btn')?.addEventListener('click', () => {
    chrome.send('xenonLoginClose', []);
  });
});

// Background service worker
chrome.runtime.onInstalled.addListener(() => {
  console.log('Xenon Overlay Extension Installed');
  if (chrome.xenonPrivate && chrome.xenonPrivate.ping) {
    chrome.xenonPrivate.ping((response) => {
      console.log('chrome.xenonPrivate.ping =>', response);
    });
  }
});

// Example message listener
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === 'ping') {
    sendResponse({result: 'pong'});
  }
});

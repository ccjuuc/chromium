// Background service worker
chrome.runtime.onInstalled.addListener(() => {
  console.log('Xenon Overlay Extension Installed');
});

// Example message listener
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === 'ping') {
    sendResponse({result: 'pong'});
  }
});

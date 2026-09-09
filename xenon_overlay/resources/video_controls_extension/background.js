/** @type {ReadonlySet<string>} */
const KINDS = new Set([
  'download',
  'smooth_play',
  'playback_speed_menu',
  'playback_speed_change',
  'picture_in_picture',
]);

const DEFAULT_BUTTONS = {xl1: true, xl2: true, xl3: true, xr1: true};

chrome.runtime.onInstalled.addListener(async () => {
  const existing = await chrome.storage.local.get('xlVideoCrxButtons');
  if (!existing.xlVideoCrxButtons) {
    await chrome.storage.local.set({xlVideoCrxButtons: DEFAULT_BUTTONS});
  }
});

/**
 * @param {string} label
 * @param {object} message
 */
function logEvent(label, message) {
  console.info('[video-crx]', label, {
    pageUrl: message.pageUrl,
    buttonId: message.buttonId,
    url: message.url ?? message.video?.currentSrc,
    playbackRate: message.playbackRate ?? message.video?.playbackRate,
  });
}

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (!message || message.source !== 'xl-video-crx') {
    return;
  }

  if (!KINDS.has(message.kind)) {
    return;
  }

  logEvent(message.kind, message);
  sendResponse({ok: true});
});

/**
 * Popup / options: read or update side-button visibility config.
 * chrome.runtime.sendMessage({source:'xl-video-crx-sw', cmd:'getButtonConfig'})
 */
chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (!message || message.source !== 'xl-video-crx-sw') {
    return;
  }
  if (message.cmd === 'getButtonConfig') {
    chrome.storage.local.get('xlVideoCrxButtons').then((data) => {
      sendResponse({
        ok: true,
        config: data.xlVideoCrxButtons ?? DEFAULT_BUTTONS,
      });
    });
    return true;
  }
  if (message.cmd === 'setButtonConfig') {
    const config = message.config ?? {};
    chrome.storage.local.set({xlVideoCrxButtons: config}).then(async () => {
      const tabs = await chrome.tabs.query({});
      for (const tab of tabs) {
        if (tab.id != null) {
          chrome.tabs
            .sendMessage(tab.id, {
              source: 'xl-video-crx-config',
              cmd: 'setButtonConfig',
              config,
            })
            .catch(() => {});
        }
      }
      sendResponse({ok: true, config});
    });
    return true;
  }
  return;
});

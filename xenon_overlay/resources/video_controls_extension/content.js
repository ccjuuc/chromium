/**
 * XL built-in video side controls bridge (content script).
 *
 * Browser (Blink shadow controls, FOR_XL):
 *   - Dispatches cancelable `xlcontrolaction` on <video> with `data-xl-control`
 *     set to xl1|xl2|xl3|xr1 before default actions.
 *   - Left buttons (xl1/xl2/xl3) are hidden until the extension sets:
 *       data-xl-download-btn-show
 *       data-xl-smooth-btn-show
 *       data-xl-speed-btn-show
 *   - PiP (xr1) is visible by default; hide with data-xl-pip-btn-show="false".
 *
 * Extension:
 *   - Opts videos into showing buttons via the attributes above.
 *   - Relays xlcontrolaction / ratechange to the service worker (logging).
 *   - Does not intercept defaults: xl1 download / xl3 speed / xr1 PiP use
 *     browser built-ins unless the page calls preventDefault on the event.
 */
(function () {
  'use strict';

  /** @typedef {'download'|'smooth_play'|'playback_speed_menu'|'picture_in_picture'|'playback_speed_change'} XlControlKind */

  /** @typedef {{xl1: boolean, xl2: boolean, xl3: boolean, xr1: boolean}} XlButtonFlags */

  /** @type {Readonly<Record<string, XlControlKind>>} */
  const BUTTON_KIND = Object.freeze({
    xl1: 'download',
    xl2: 'smooth_play',
    xl3: 'playback_speed_menu',
    xr1: 'picture_in_picture',
  });

  /** Maps side button id -> video attribute that controls visibility. */
  const SHOW_ATTR = Object.freeze({
    xl1: 'data-xl-download-btn-show',
    xl2: 'data-xl-smooth-btn-show',
    xl3: 'data-xl-speed-btn-show',
    xr1: 'data-xl-pip-btn-show',
  });

  const STORAGE_KEY = 'xlVideoCrxButtons';

  /** @type {XlButtonFlags} */
  const DEFAULT_BUTTONS = Object.freeze({
    xl1: true,
    xl2: true,
    xl3: true,
    xr1: true,
  });

  const BOUND = Symbol('xlVideoCrxBound');
  const LAST_RATE = Symbol('xlVideoCrxLastRate');

  /** @type {XlButtonFlags} */
  let buttonConfig = {...DEFAULT_BUTTONS};

  /**
   * @param {HTMLVideoElement} video
   * @returns {object}
   */
  function snapshotVideo(video) {
    return {
      currentSrc: video.currentSrc || video.src || '',
      playbackRate: video.playbackRate,
      paused: video.paused,
      muted: video.muted,
      volume: video.volume,
    };
  }

  /**
   * @param {HTMLVideoElement} video
   * @param {XlButtonFlags} flags
   */
  function applyButtonVisibility(video, flags) {
    for (const [id, attr] of Object.entries(SHOW_ATTR)) {
      const on = Boolean(flags[id]);
      if (on) {
        video.setAttribute(attr, '');
      } else {
        video.setAttribute(attr, 'false');
      }
    }
  }

  /**
   * @param {XlControlKind} kind
   * @param {HTMLVideoElement} video
   * @param {object} extra
   */
  function emit(kind, video, extra = {}) {
    chrome.runtime.sendMessage({
      source: 'xl-video-crx',
      kind,
      pageUrl: location.href,
      video: snapshotVideo(video),
      timestamp: Date.now(),
      ...extra,
    });
  }

  /**
   * @param {HTMLVideoElement} video
   */
  function bindVideo(video) {
    if (!(video instanceof HTMLVideoElement) || video[BOUND]) {
      return;
    }
    video[BOUND] = true;
    video[LAST_RATE] = video.playbackRate;

    applyButtonVisibility(video, buttonConfig);

    video.addEventListener('xlcontrolaction', (event) => {
      const buttonId = video.getAttribute('data-xl-control') || '';
      const kind = BUTTON_KIND[buttonId];
      if (!kind) {
        return;
      }

      emit(kind, video, {
        buttonId,
        defaultPrevented: event.defaultPrevented,
      });
    });

    video.addEventListener('ratechange', () => {
      const prev = video[LAST_RATE];
      const next = video.playbackRate;
      if (prev === next) {
        return;
      }
      video[LAST_RATE] = next;
      emit('playback_speed_change', video, {
        previousRate: prev,
        playbackRate: next,
      });
    });
  }

  /**
   * @param {ParentNode} root
   */
  function scan(root) {
    if (!root || !root.querySelectorAll) {
      return;
    }
    root.querySelectorAll('video').forEach((v) => bindVideo(v));
  }

  /**
   * Re-apply visibility flags to every bound video (e.g. after config change).
   * @param {XlButtonFlags} flags
   */
  function refreshAllVideos(flags) {
    document.querySelectorAll('video').forEach((video) => {
      if (video[BOUND]) {
        applyButtonVisibility(video, flags);
      } else {
        bindVideo(video);
      }
    });
  }

  /**
   * @param {Partial<XlButtonFlags>} patch
   * @returns {Promise<XlButtonFlags>}
   */
  async function setButtonConfig(patch) {
    buttonConfig = {...buttonConfig, ...patch};
    refreshAllVideos(buttonConfig);
    try {
      await chrome.storage.local.set({[STORAGE_KEY]: buttonConfig});
    } catch (e) {
      console.warn('[video-crx] storage save failed', e);
    }
    return {...buttonConfig};
  }

  /**
   * @returns {Promise<XlButtonFlags>}
   */
  async function loadButtonConfig() {
    try {
      const stored = await chrome.storage.local.get(STORAGE_KEY);
      if (stored && stored[STORAGE_KEY]) {
        buttonConfig = {...DEFAULT_BUTTONS, ...stored[STORAGE_KEY]};
      }
    } catch (e) {
      console.warn('[video-crx] storage load failed', e);
    }
    return {...buttonConfig};
  }

  /** Page / devtools helper (content-script world). */
  globalThis.__xlVideoCrx = Object.freeze({
    getButtonConfig: () => ({...buttonConfig}),
    setButtonConfig,
    /** @param {HTMLVideoElement} video @param {Partial<XlButtonFlags>} flags */
    setVideoButtons(video, flags) {
      if (!(video instanceof HTMLVideoElement)) {
        throw new TypeError('expected HTMLVideoElement');
      }
      const merged = {...buttonConfig, ...flags};
      applyButtonVisibility(video, merged);
      if (!video[BOUND]) {
        bindVideo(video);
      }
    },
    rescan: () => scan(document),
    SHOW_ATTR,
    BUTTON_KIND,
  });

  (async function init() {
    await loadButtonConfig();

    const start = () => {
      scan(document);
      const observer = new MutationObserver((mutations) => {
        for (const mutation of mutations) {
          for (const node of mutation.addedNodes) {
            if (node instanceof HTMLVideoElement) {
              bindVideo(node);
            } else if (node instanceof Element) {
              scan(node);
            }
          }
        }
      });
      observer.observe(document.documentElement, {
        childList: true,
        subtree: true,
      });
    };

    if (document.documentElement) {
      start();
    } else {
      document.addEventListener('DOMContentLoaded', start, {once: true});
    }
  })();

  chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    if (!message || message.source !== 'xl-video-crx-config') {
      return;
    }
    if (message.cmd === 'getButtonConfig') {
      sendResponse({ok: true, config: {...buttonConfig}});
      return;
    }
    if (message.cmd === 'setButtonConfig') {
      setButtonConfig(message.config || {})
        .then((config) => sendResponse({ok: true, config}))
        .catch((error) =>
          sendResponse({ok: false, error: String(error)}),
        );
      return true;
    }
    return;
  });
})();

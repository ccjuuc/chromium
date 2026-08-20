// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* eslint-disable @typescript-eslint/require-await */
/* eslint-disable @typescript-eslint/no-unnecessary-type-assertion */
/* eslint-disable @typescript-eslint/no-explicit-any */

import {ipcBroadcast, registeredRpcFunctions} from './net_ipc.js';
import {
  activePlayList,
  activePlayListAddListeners,
  activePlayListManager,
  activePlayListSelectListeners,
  populateSiblingVideos,
  setCurrentPlayingPlayListId,
  setPlayItemHandler,
} from './playlist_service.js';

export let globalDropFilesListener: ((f: string) => void) | null = null;
export let currentPlayingUrl = '';
export function setCurrentPlayingUrl(url: string) {
  currentPlayingUrl = url;
}
export const fileNameByBlobUrl = new Map<string, string>();
export const blobUrlByFileName = new Map<string, string>();

export let realPcAddon: any = null;
export let realAplayerStackInstance: any = null;
let currentStackInstance: any = null;
let currentDummyMediaTarget: any = null;
// The wrapper stack is a singleton: the original impl-layer AplayerStack
// constructs one instance (Vue's pause/play goes through it) while
// playLocalFile() constructs another to run openMedia().  With two instances,
// Vue's attachPlayStateChangeEvent callbacks landed on the openMedia
// instance's listener set while pauseMedia() fired the impl instance's empty
// set, so the native pause worked but the play/pause button never updated.
let existingStackProxy: NativeAplayerStack | null = null;
export let currentVolume = 100;
let currentSilent = false;

let realPcAddonReadyPromise: Promise<any>|null = null;

export function setRealPcAddon(addon: any) {
  realPcAddon = addon;
}

/**
 * Resolves once Mojo-backed pc_addon.node is loaded and assigned. Play paths
 * must await this — otherwise the sync Electron mock fakes onFirstRender with
 * no player_wnd under the video host.
 */
export function waitForRealPcAddon(timeoutMs = 30000): Promise<any> {
  if (realPcAddon) {
    return Promise.resolve(realPcAddon);
  }
  if (!realPcAddonReadyPromise) {
    return Promise.reject(new Error('pc_addon load was not started'));
  }
  return Promise.race([
    realPcAddonReadyPromise,
    new Promise<any>((_resolve, reject) => {
      window.setTimeout(
          () => reject(new Error(`pc_addon.node load timed out after ${
              timeoutMs}ms`)),
          timeoutMs);
    }),
  ]);
}

export function trackRealPcAddonLoad(load: Promise<any>): Promise<any> {
  realPcAddonReadyPromise = load.then((addon) => {
    if (!addon) {
      throw new Error('pc_addon.node loaded empty exports');
    }
    setRealPcAddon(addon);
    return addon;
  });
  realPcAddonReadyPromise.catch(() => {});
  return realPcAddonReadyPromise;
}

async function applyRealStackVolume(volume: number): Promise<void> {
  if (!realAplayerStackInstance) {
    return;
  }

  if (volume > 0 &&
      typeof realAplayerStackInstance.setSilent === 'function') {
    // pc_addon's Electron wrapper normalizes this API to 0/1. Keep the same
    // contract here instead of relying on a native bool conversion.
    await realAplayerStackInstance.setSilent(0);
    currentSilent = false;
  }
  if (typeof realAplayerStackInstance.setVolume === 'function') {
    await realAplayerStackInstance.setVolume(volume);
  }
}

function reapplyRealStackVolumeAfter(delayMs: number): void {
  const stack = realAplayerStackInstance;
  window.setTimeout(() => {
    if (!stack || realAplayerStackInstance !== stack) {
      return;
    }
    void applyRealStackVolume(currentVolume).catch((err: unknown) => {
      console.warn('[xenon-player] delayed native volume sync failed:', err);
    });
  }, delayMs);
}

function createDummyManager(mediaName = 'video.mp4', mediaUrl = '', videoEl?: HTMLVideoElement | null): object {
  const listeners: Record<string, ((...args: unknown[]) => unknown)[]> = {};
  const target: Record<string, unknown> = {
    play: () => {
      if (videoEl) { videoEl.play().catch(() => {}); }
      else if (currentStackInstance) { currentStackInstance.playMedia().catch(() => {}); }
      return 0;
    },
    pause: () => {
      if (videoEl) { videoEl.pause(); }
      else if (currentStackInstance) { currentStackInstance.pauseMedia().catch(() => {}); }
      return 0;
    },
    close: () => { if (videoEl) { videoEl.pause(); videoEl.src = ''; } return 0; },
    getName: () => mediaName,
    getUrl: () => mediaUrl,
    getId: () => 'media_' + Date.now(),
    getState: () => 4,
    getPosition: () => videoEl ? Math.floor(videoEl.currentTime * 1000) : 0,
    getDuration: () => videoEl && Number.isFinite(videoEl.duration) ? Math.floor(videoEl.duration * 1000) : 60000,
    getPlayList: () => activePlayListManager,
  };
  return new Proxy(target, {
    get: (t, p) => {
      if (typeof p === 'string' && p in t) return t[p];
      if (typeof p === 'string' && p.startsWith('attach')) {
        return (cb: (...args: unknown[]) => unknown) => {
          listeners[p] = listeners[p] || [];
          listeners[p].push(cb);
          return listeners[p].length;
        };
      }
      if (typeof p === 'string' && p.startsWith('detach')) {
        return () => undefined;
      }
      if (typeof p === 'string' && (p.includes('Manager') || p.startsWith('get') || p.startsWith('set') || p.startsWith('init'))) {
        return () => createDummyManager(mediaName, mediaUrl, videoEl);
      }
      return () => 0;
    },
    apply: () => 0,
  });
}

const allMediaListeners = new Set<(media: unknown) => void>();

export class NativeAplayerStack {
  private mediaId = 'media_main';
  private mediaState = 4; // 4 = MsPlay
  private position = 0;
  private duration = 0;
  private timer: number | null = null;
  private mediaGeneration = 0;
  private stateListeners = new Set<(state: number) => void>();

  constructor() {
    if (existingStackProxy) {
      return existingStackProxy;
    }
    currentStackInstance = this;
    const proxy = new Proxy(this, {
      get: (target, prop) => {
        if (typeof prop === 'string' && prop in target) {
          return (target as Record<string, unknown>)[prop];
        }
        if (prop === 'getPlayList') {
          return () => activePlayListManager;
        }
        if (typeof prop === 'string' && prop.startsWith('attach')) {
          return () => 1;
        }
        if (typeof prop === 'string' && prop.startsWith('detach')) {
          return () => undefined;
        }
        if (prop === 'getCurrPlayMedia') {
          return () => currentDummyMediaTarget || createDummyManager();
        }
        if (typeof prop === 'string' && (prop.includes('Manager') || prop.startsWith('get') || prop.startsWith('set') || prop.startsWith('init'))) {
          return () => createDummyManager();
        }
        return (...args: unknown[]) => {
          const cb = args.find(a => typeof a === 'function') as ((...args: unknown[]) => unknown) | undefined;
          if (cb) {
            queueMicrotask(() => cb(createDummyManager()));
          }
          return 0;
        };
      },
    });
    existingStackProxy = proxy;
    return proxy;
  }

  setWnd(hwnd: unknown) {
    if (realPcAddon && typeof realPcAddon.setWnd === 'function') {
      return realPcAddon.setWnd(hwnd);
    }
    return 0;
  }

  setWndEx(floatWnd: unknown, parentWnd: unknown) {
    if (realPcAddon && typeof realPcAddon.setWndEx === 'function') {
      return realPcAddon.setWndEx(floatWnd, parentWnd);
    }
    return 0;
  }

  async createPlayerWnd(floatWnd: unknown, parentWnd: unknown) {
    this.setWndEx(floatWnd, parentWnd);
    return new Promise(resolve => {
      this.getAplayerWnd((hwnd: unknown) => resolve(hwnd));
    });
  }

  getAplayerWnd(cb?: (hwnd: unknown) => void) {
    if (realPcAddon && typeof realPcAddon.getAplayerWnd === 'function') {
      return realPcAddon.getAplayerWnd(cb);
    }
    const dummyHwnd = (window as any).__xenonPlayerHandles__?.parentWindow || '0';
    if (typeof cb === 'function') {
      queueMicrotask(() => cb(dummyHwnd));
    }
    return dummyHwnd;
  }

  getPlayList() {
    return activePlayListManager;
  }

  getImageRatioItems() {
    return [
      { id: 0, name: '默认' },
      { id: 1, name: '16:9' },
      { id: 2, name: '4:3' },
      { id: 3, name: '铺满' },
    ];
  }

  setVolume(vol: number) {
    currentVolume = typeof vol === 'number' ?
        Math.max(0, Math.min(100, vol)) :
        100;
    if (realAplayerStackInstance && typeof realAplayerStackInstance.setVolume === 'function') {
      try {
        const result = applyRealStackVolume(currentVolume)
            .catch((err: unknown) => {
              console.warn(
                  '[xenon-player] failed to apply native volume:', err);
            });
        // The engine can reset its mixer while the first audio renderer is
        // being created. Re-apply the latest (not the captured) slider value
        // after that transition so a visible non-zero slider cannot leave the
        // native mixer at zero.
        reapplyRealStackVolumeAfter(250);
        return result;
      } catch (err) {
        console.warn('[xenon-player] failed to apply native volume:', err);
      }
    }
    if (realPcAddon && typeof realPcAddon.setVolume === 'function') {
      try {
        realPcAddon.setVolume(currentVolume);
      } catch {}
    }
    return undefined;
  }

  getVolume(cb?: (volume: number) => void) {
    if (realAplayerStackInstance && typeof realAplayerStackInstance.getVolume === 'function') {
      try {
        let callbackInvoked = false;
        let fallbackTimer: number|undefined;
        const onVolume = (volume: unknown) => {
          if (callbackInvoked) {
            return;
          }
          callbackInvoked = true;
          if (fallbackTimer !== undefined) {
            window.clearTimeout(fallbackTimer);
          }
          const numericVolume = Number(volume);
          if (Number.isFinite(numericVolume)) {
            if (numericVolume > 0 || currentVolume === 0) {
              currentVolume = numericVolume;
            } else {
              // During first-frame setup pc_addon can briefly report zero even
              // after accepting setVolume. Preserve and re-apply the requested
              // non-zero value instead of feeding the transient zero back into
              // Vue's volume state.
              reapplyRealStackVolumeAfter(0);
            }
          }
          cb?.(currentVolume);
        };
        fallbackTimer = window.setTimeout(() => onVolume(currentVolume), 500);
        return Promise.resolve(realAplayerStackInstance.getVolume(onVolume))
            .catch((err: unknown) => {
              console.warn(
                  '[xenon-player] realAplayerStackInstance.getVolume error:',
                  err);
              onVolume(currentVolume);
              return currentVolume;
            });
      } catch (err) {
        console.warn(
            '[xenon-player] realAplayerStackInstance.getVolume error:', err);
      }
    }
    queueMicrotask(() => cb?.(currentVolume));
    return currentVolume;
  }

  isSilent(cb?: (silent: boolean) => void) {
    if (realAplayerStackInstance &&
        typeof realAplayerStackInstance.isSilent === 'function') {
      try {
        let callbackInvoked = false;
        let fallbackTimer: number|undefined;
        const onSilent = (silent: unknown) => {
          if (callbackInvoked) {
            return;
          }
          callbackInvoked = true;
          if (fallbackTimer !== undefined) {
            window.clearTimeout(fallbackTimer);
          }
          currentSilent = Boolean(silent);
          cb?.(currentSilent);
        };
        fallbackTimer = window.setTimeout(() => onSilent(currentSilent), 500);
        return Promise.resolve(realAplayerStackInstance.isSilent(onSilent))
            .catch((err: unknown) => {
              console.warn(
                  '[xenon-player] realAplayerStackInstance.isSilent error:',
                  err);
              onSilent(currentSilent);
              return currentSilent;
            });
      } catch (err) {
        console.warn(
            '[xenon-player] realAplayerStackInstance.isSilent error:', err);
      }
    }
    queueMicrotask(() => cb?.(currentSilent));
    return currentSilent;
  }

  setSilent(silent: boolean|number) {
    currentSilent = Boolean(silent);
    if (realAplayerStackInstance &&
        typeof realAplayerStackInstance.setSilent === 'function') {
      try {
        return Promise.resolve(
            realAplayerStackInstance.setSilent(+currentSilent))
            .catch((err: unknown) => {
              console.warn(
                  '[xenon-player] realAplayerStackInstance.setSilent error:',
                  err);
            });
      } catch (err) {
        console.warn(
            '[xenon-player] realAplayerStackInstance.setSilent error:', err);
      }
    }
    return undefined;
  }

  setAudioNormalize(enable: boolean) {
    if (realAplayerStackInstance && typeof realAplayerStackInstance.setAudioNormalize === 'function') {
      try {
        realAplayerStackInstance.setAudioNormalize(enable);
      } catch {}
    }
  }

  useNormalize(enable: boolean) {
    this.setAudioNormalize(enable);
  }

  static openMediaHooks: Array<(attr: any) => Promise<void>> = [];

  setMediaOpenHook(cb: (attr: any) => Promise<void>) {
    if (typeof cb === 'function') {
      NativeAplayerStack.openMediaHooks.push(cb);
    }
  }

  addCloseMediaHook(_cb: () => void) {}

  attachMediaChangeEvent(cb: (media: unknown) => void) {
    if (typeof cb === 'function') {
      allMediaListeners.add(cb);
    }
    return 1;
  }

  detachMediaChangeEvent(cbOrCookie: unknown) {
    if (typeof cbOrCookie === 'function') {
      allMediaListeners.delete(cbOrCookie as (media: unknown) => void);
    }
  }

  attachMediaClosedEvent(_cb: unknown) {
    return 1;
  }
  detachMediaCloseEvent(_cookie: unknown) {}
  attachQuitPlayEvent(_cb: unknown) {
    return 1;
  }
  detachQuitPlayEvent(_cookie: unknown) {}
  attachInitFinishEvent(_cb: unknown) {
    return 1;
  }
  detachInitFinishEvent(_cookie: unknown) {}
  attachPlayerCrashEvent(_cb: unknown) {
    return 1;
  }
  detachPlayerCrashEvent(_cookie: unknown) {}
  attachDropFilesEvent(cb: (files: string) => void) {
    if (typeof cb === 'function') {
      globalDropFilesListener = cb;
    }
    return 1;
  }
  detachDropFilesEvent(_cookie: unknown) {}

  attachRenderFirstFrameEvent(cb: () => void) {
    // Do not fake first-frame while a real native surface is expected — that
    // made AplayerWndShow fire with an empty video host (no player_wnd).
    if (realPcAddon) {
      return 1;
    }
    if (typeof cb === 'function') {
      window.setTimeout(() => {
        try {
          cb();
        } catch {
        }
      }, 50);
    }
    return 1;
  }

  attachRadioChangeEvent(cb: (w: number, h: number) => void) {
    if (typeof cb === 'function') {
      window.setTimeout(() => { try { cb(16, 9); } catch {} }, 60);
    }
    return 1;
  }

  attachPlayStateChangeEvent(cb: (state: number) => void) {
    if (typeof cb === 'function') {
      window.setTimeout(() => {
        try {
          cb(2); // MsSucc
          window.setTimeout(() => { try { cb(4); } catch {} }, 50); // MsPlay
        } catch {}
      }, 50);
    }
    return 1;
  }

  attachBufferingEvent(_cb: unknown) { return 1; }
  attachSubtitleEvent(_cb: unknown) { return 1; }
  attachAudioStreamEvent(_cb: unknown) { return 1; }
  attachVideoStreamEvent(_cb: unknown) { return 1; }
  attachPlayEndedEvent(_cb: unknown) { return 1; }

  async openMedia(mediaAttr: any = {}) {
    const mediaGeneration = ++this.mediaGeneration;
    if (this.timer) {
      window.clearInterval(this.timer);
      this.timer = null;
    }
    this.position = 0;
    this.duration = 0;

    if (NativeAplayerStack.openMediaHooks.length > 0) {
      try {
        await Promise.all(NativeAplayerStack.openMediaHooks.map(hook => {
          try {
            return hook(mediaAttr);
          } catch (e) {
            console.error('[xenon-player] openMediaHook error:', e);
            return Promise.resolve();
          }
        }));
      } catch (e) {
        console.error('[xenon-player] openMediaHooks error:', e);
      }
    }

    try {
      (window as any).electron?.ipcRenderer?.send?.('AplayerWndShow');
      const home = document.querySelector<HTMLElement>('.home-container');
      if (home) home.style.display = 'none';
      const player = document.querySelector<HTMLElement>('.xmp-player-container');
      if (player) {
        player.style.display = 'block';
        player.classList.remove('dev-hide');
      }
    } catch {}

    const rawPlayUrl = (mediaAttr.playUrl && mediaAttr.playUrl.trim()) ||
                       mediaAttr.task?.url ||
                       mediaAttr.web?.url ||
                       currentPlayingUrl;
    const playUrl = blobUrlByFileName.get(rawPlayUrl) || rawPlayUrl || '';
    const name = mediaAttr.name || fileNameByBlobUrl.get(playUrl) || (playUrl ? playUrl.split('/').pop() : 'video.mp4') || 'video.mp4';

    // Ensure any existing injected video element is cleaned up from DOM
    const oldVideo = document.getElementById('xenon-html5-video');
    if (oldVideo) {
      oldVideo.remove();
    }

    // Add to activePlayList if not existing
    let existingItem = activePlayList.find(i => i.url === (rawPlayUrl || playUrl));
    if (!existingItem) {
      const newItem = {
        id: 'item_' + Date.now() + '_' + Math.random().toString(36).substring(2, 7),
        name,
        url: rawPlayUrl || playUrl,
        duration: 0,
        mediaType: mediaAttr.mediaType || ((rawPlayUrl && rawPlayUrl.startsWith('http')) ? 3 : 5),
      };
      activePlayList.push(newItem);
      existingItem = newItem;
      activePlayListAddListeners.forEach(cb => { try { cb(true, newItem); } catch {} });
    }
    setCurrentPlayingPlayListId(existingItem.id);
    activePlayListSelectListeners.forEach(cb => { try { cb(existingItem!.id); } catch {} });

    // Auto scan directory if local video
    if (rawPlayUrl && !rawPlayUrl.startsWith('http://') && !rawPlayUrl.startsWith('https://')) {
      populateSiblingVideos(rawPlayUrl || playUrl);
    }

    // Declared before the native wiring below so the engine's push events
    // (attachProgressChangedEvent) can forward into the same listener set the
    // dummyMedia attach APIs hand to Vue.
    const progressListeners = new Set<(pos: number) => void>();
    this.stateListeners.clear();
    const stateListeners = this.stateListeners;
    let nativeProgressPushed = false;
    const publishProgress = () => {
      const currentPos = this.position;
      for (const cb of progressListeners) {
        try { cb(currentPos); } catch {}
      }
      ipcBroadcast('AplayerMeidaProgressChange', this.mediaId, currentPos);
    };

    if (realPcAddon && typeof realPcAddon.NativeAplayerStack === 'function') {
      try {
        if (realAplayerStackInstance && typeof realAplayerStackInstance.$dispose === 'function') {
          realAplayerStackInstance.$dispose().catch(() => {});
        }
        const handles = (window as any).__xenonPlayerHandles__;
        if (handles && typeof realPcAddon.setWndEx === 'function') {
          await realPcAddon.setWndEx(
              Number(handles.floatWindow), Number(handles.parentWindow));
        }
        realAplayerStackInstance = new realPcAddon.NativeAplayerStack();
        if (realAplayerStackInstance && realAplayerStackInstance.__xenonReady) {
          await realAplayerStackInstance.__xenonReady;
        }
        // Wire push events before opening so the first MediaChangeEvent is
        // not missed (the original impl attaches in its constructor too).
        this.wireRealStackEvents(mediaGeneration, (pos: number) => {
          nativeProgressPushed = true;
          this.position = pos;
          publishProgress();
        });
        const realAttr = {
          name,
          gcid: '',
          playUrl: rawPlayUrl || playUrl,
          playFrom: 'xenon-player',
          zipPlay: 0,
          dlnaPlay: 0,
          mediaType: (rawPlayUrl && rawPlayUrl.startsWith('http')) ? 3 : 5,
          setPos: 0,
          position: 0,
          timingAttributes: '',
          callOpenMediaTiming: '',
          pan: {},
          task: {},
          nas: {},
          scrape: {},
          universal: {},
        };
        const openResult = await realAplayerStackInstance.openMedia(realAttr);
        console.info('[xenon-player] real pc_addon openMedia ok', openResult);
        await applyRealStackVolume(currentVolume);
        reapplyRealStackVolumeAfter(250);
        reapplyRealStackVolumeAfter(1000);
        // Native first-frame may not reach Vue's mock attach hooks — show the
        // embedded host and player chrome once openMedia succeeds.
        try {
          (window as any).electron?.ipcRenderer?.send?.('AplayerWndShow');
        } catch {}
      } catch (err) {
        console.warn('[xenon-player] failed to open via real pc_addon NativeAplayerStack:', err);
      }
    }

    this.mediaId = 'media_' + Date.now();
    const dummyMediaTarget: Record<string, unknown> = {
      id: this.mediaId,
      name,
      url: playUrl,
      duration: this.duration,
      getName: () => name,
      getType: () => mediaAttr.mediaType || ((rawPlayUrl && rawPlayUrl.startsWith('http')) ? 3 : 5),
      getAttribute: () => ({
        name,
        playUrl: rawPlayUrl || playUrl,
        mediaType: mediaAttr.mediaType || ((rawPlayUrl && rawPlayUrl.startsWith('http')) ? 3 : 5),
        gcid: mediaAttr.gcid || '',
        task: mediaAttr.task || {},
        web: mediaAttr.web || {},
        pan: mediaAttr.pan || {},
      }),
      getExtraAttribute: () => ({}),
      getMediaWidth: () => 1920,
      getMediaHeight: () => 1080,
      getIsLive: () => false,
      isTaskLocalPlay: () => false,
      isPanPlay: () => false,
      isTaskPlay: () => false,
      getAudioTrackList: () => [],
      getAudioTrackSelectedIndex: () => 0,
      getRatioList: () => [
        { id: 0, name: '默认' },
        { id: 1, name: '16:9' },
        { id: 2, name: '4:3' },
        { id: 3, name: '铺满' },
      ],
      getRatioSelectedId: () => 0,
      getSubtitleList: () => [],
      getSubtitleSelectedIndex: () => -1,
      getSubtitleManager: () => createDummyManager(),
      getState: () => this.mediaState,
      getMediaState: () => this.mediaState,
      getPosition: () => this.position,
      getPlayProgress: () => this.position,
      getDuration: () => this.duration,
      play: () => this.play(),
      pause: () => this.pause(),
      setPosition: (pos: number) => this.setPosition(pos),
      progressMoveTo: (pos: number) => this.setPosition(pos),
      attachPlayStateChangeEvent: (cb: (s: number) => void) => {
        if (typeof cb === 'function') {
          stateListeners.add(cb);
          window.setTimeout(() => {
            try {
              cb(2); // 2 = MsSucc
              window.setTimeout(() => { try { cb(4); } catch {} }, 30); // 4 = MsPlay
            } catch {}
          }, 20);
        }
        return 1;
      },
      detachPlayStateChangeEvent: (cb: (s: number) => void) => {
        stateListeners.delete(cb);
      },
      attachProgressChangedEvent: (cb: (pos: number) => void) => {
        if (typeof cb === 'function') {
          progressListeners.add(cb);
          window.setTimeout(() => { try { cb(this.position); } catch {} }, 10);
        }
        return 1;
      },
      detachProgressChangedEvent: (cb: (pos: number) => void) => {
        progressListeners.delete(cb);
      },
      attachProgressChangeEvent: (cb: (pos: number) => void) => {
        if (typeof cb === 'function') {
          progressListeners.add(cb);
        }
        return 1;
      },
      detachProgressChangeEvent: (cb: (pos: number) => void) => {
        progressListeners.delete(cb);
      },
      attachFirstRenderEvent: (cb: () => void) => {
        if (typeof cb === 'function') {
          window.setTimeout(() => { try { cb(); } catch {} }, 30);
        }
        return 1;
      },
      attachHDRPreparedEvent: (_cb: unknown) => 1,
      attachPlayBufferEvent: (_cb: unknown) => 1,
    };

    const dummyMedia = new Proxy(dummyMediaTarget, {
      get: (tgt, prop) => {
        if (typeof prop === 'string' && prop in tgt) {
          return tgt[prop];
        }
        if (typeof prop === 'string' && (prop.includes('Manager') || prop.startsWith('get') || prop.startsWith('is'))) {
          return () => createDummyManager();
        }
        if (typeof prop === 'string' && prop.startsWith('attach')) {
          return () => 1;
        }
        if (typeof prop === 'string' && prop.startsWith('detach')) {
          return () => undefined;
        }
        return () => undefined;
      },
    });

    currentDummyMediaTarget = dummyMedia;
    for (const listener of allMediaListeners) {
      try { listener(dummyMedia); } catch (e) { console.error('[xenon-player mediaListener error]:', e); }
    }

    // Broadcast IPC events for Vue UI
    ipcBroadcast('AplayerStackMediaChangeEvent', dummyMediaTarget['id']);
    ipcBroadcast('AplayerMeidaFirstRender', dummyMediaTarget['id'], 0);
    ipcBroadcast('AplayerMeidaRatioPrepared', dummyMediaTarget['id']);
    ipcBroadcast('AplayerMeidaPlayStateChange', dummyMediaTarget['id'], 2); // MsSucc
    ipcBroadcast('AplayerPlayListItemAdd', true, existingItem);
    ipcBroadcast('AplayerPlayListPrepared');
    ipcBroadcast('AplayerPlayListSelectChange', existingItem.id);

    window.setTimeout(() => {
      if (mediaGeneration === this.mediaGeneration) {
        this.publishMediaState(4); // MsPlay
      }
    }, 50);

    let progressPollPending = false;
    let progressReadErrorLogged = false;
    // The interval is now a fallback only: while the engine pushes progress
    // (nativeProgressPushed) it merely keeps syncing the duration; without a
    // native player it keeps the mock clock, and if pushes never arrive it
    // resumes polling position itself.
    const pollProgress = async () => {
      if (progressPollPending || mediaGeneration !== this.mediaGeneration) {
        return;
      }
      progressPollPending = true;
      try {
        const stack = realAplayerStackInstance;
        if (stack && typeof stack.$invokePath === 'function') {
          if (this.duration <= 0) {
            const duration = Number(await stack.$invokePath(
                'getCurrPlayMedia.getDuration'));
            if (mediaGeneration !== this.mediaGeneration) {
              return;
            }
            if (Number.isFinite(duration) && duration > 0) {
              this.duration = duration;
              dummyMediaTarget['duration'] = duration;
              existingItem!.duration = duration;
            }
          }
          if (!nativeProgressPushed) {
            const position = Number(await stack.$invokePath(
                'getCurrPlayMedia.getPlayProgress'));
            if (mediaGeneration !== this.mediaGeneration) {
              return;
            }
            if (Number.isFinite(position) && position >= 0) {
              this.position = position;
            }
            publishProgress();
          }
          progressReadErrorLogged = false;
          return;
        }

        // Keep the old mock clock only when no native player is available.
        // Once pc_addon is active, retaining the last confirmed native value is
        // more truthful than fabricating forward progress during a read error.
        if (!realPcAddon && this.mediaState === 4) {
          if (this.duration <= 0) {
            this.duration = 60000;
            dummyMediaTarget['duration'] = this.duration;
          }
          this.position += 500;
          publishProgress();
        }
      } catch (err) {
        if (!progressReadErrorLogged) {
          progressReadErrorLogged = true;
          console.warn(
              '[xenon-player] failed to read native playback progress:', err);
        }
      } finally {
        progressPollPending = false;
      }
    };

    void pollProgress();
    this.timer = window.setInterval(() => void pollProgress(), 500);

    return { result: 0 };
  }

  async closeMedia() {
    if (currentStackInstance && currentStackInstance !== this) {
      return currentStackInstance.closeMedia();
    }
    ++this.mediaGeneration;
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
    if (realAplayerStackInstance) {
      try {
        if (typeof realAplayerStackInstance.closeMedia === 'function') {
          await realAplayerStackInstance.closeMedia();
        }
      } catch (error) {
        console.warn('[xenon-player] native closeMedia failed:', error);
      }
      if (typeof realAplayerStackInstance.$dispose === 'function') {
        realAplayerStackInstance.$dispose().catch(() => {});
      }
      realAplayerStackInstance = null;
    }
    this.mediaState = 0;
    this.stateListeners.clear();
    // Keep currentStackInstance registered: the wrapper is a singleton, so
    // later RPC/UI calls (e.g. a re-openMedia) must still route to it.
    return { result: 0 };
  }

  private publishMediaState(state: number): void {
    this.mediaState = state;
    console.log('[xenon-player] publishMediaState:', state, 'mediaId:', this.mediaId);
    for (const listener of this.stateListeners) {
      try {
        listener(state);
      } catch {
      }
    }
    ipcBroadcast('AplayerMeidaPlayStateChange', this.mediaId, state);
  }

  /**
   * Mirrors the original Electron event flow, which is push-based: the native
   * engine invokes the callbacks registered via attachPlayStateChangeEvent on
   * its own media object (impl/aplayer-media.ts just delegates, and the
   * server layer re-broadcasts them over IPC).  The Mojo-backed pc_addon
   * carries those callbacks across the bridge (callbackArg ->
   * NodeCallbackInvoked), so forward the real engine's state pushes into our
   * listener set instead of polling getMediaState.
   */
  private wireRealStackEvents(
      mediaGeneration: number,
      onNativeProgress?: (pos: number) => void): void {
    const stack = realAplayerStackInstance;
    if (!stack || typeof stack.attachMediaChangeEvent !== 'function') {
      return;
    }
    try {
      let lastPushedState = -1;
      const onNativeState = (state: unknown) => {
        if (mediaGeneration !== this.mediaGeneration) {
          return;
        }
        const s = Number(state);
        if (!Number.isFinite(s) || s <= 0 || s === lastPushedState) {
          return;
        }
        lastPushedState = s;
        const active = currentStackInstance || this;
        active.publishMediaState(s);
      };
      stack.attachMediaChangeEvent((nativeMedia: any) => {
        if (mediaGeneration !== this.mediaGeneration) {
          return;
        }
        if (nativeMedia &&
            typeof nativeMedia.attachPlayStateChangeEvent === 'function') {
          try {
            nativeMedia.attachPlayStateChangeEvent(onNativeState);
          } catch (error) {
            console.warn(
                '[xenon-player] failed to attach native state listener:',
                error);
          }
        }
        if (onNativeProgress && nativeMedia &&
            typeof nativeMedia.attachProgressChangedEvent === 'function') {
          try {
            nativeMedia.attachProgressChangedEvent((pos: unknown) => {
              if (mediaGeneration !== this.mediaGeneration) {
                return;
              }
              const p = Number(pos);
              if (Number.isFinite(p) && p >= 0) {
                onNativeProgress(p);
              }
            });
          } catch (error) {
            console.warn(
                '[xenon-player] failed to attach native progress listener:',
                error);
          }
        }
      });
    } catch (error) {
      console.warn('[xenon-player] failed to wire native state events:', error);
    }
  }

  async playMedia() {
    const stack = realAplayerStackInstance;
    if (stack && typeof stack.playMedia === 'function') {
      try {
        await stack.playMedia();
      } catch (error) {
        console.warn('[xenon-player] native playMedia failed:', error);
      }
    }
    // Publish the user's intent immediately so the UI reacts without waiting
    // for the engine; wireRealStackEvents() then pushes the authoritative
    // state once the engine confirms the transition.
    const activeStack = currentStackInstance || this;
    activeStack.publishMediaState(4); // MsPlay
    return { result: 0 };
  }

  async pauseMedia() {
    const stack = realAplayerStackInstance;
    if (stack && typeof stack.pauseMedia === 'function') {
      try {
        await stack.pauseMedia();
      } catch (error) {
        console.warn('[xenon-player] native pauseMedia failed:', error);
      }
    }
    const activeStack = currentStackInstance || this;
    activeStack.publishMediaState(3); // MsPause
    return { result: 0 };
  }

  async play() {
    return this.playMedia();
  }

  async pause() {
    return this.pauseMedia();
  }

  async setPosition(pos: number) {
    this.position = pos;
    const stack = realAplayerStackInstance;
    if (stack && typeof stack.$invokePath === 'function') {
      try {
        await stack.$invokePath('getCurrPlayMedia.progressMoveTo', pos);
      } catch (progressMoveError) {
        try {
          await stack.$invokePath('getCurrPlayMedia.setPosition', pos);
        } catch (setPositionError) {
          console.warn(
              '[xenon-player] failed to seek native media:',
              progressMoveError, setPositionError);
        }
      }
    }
    return { result: 0 };
  }

  async getPosition() {
    return { result: 0, position: this.position };
  }

  async getDuration() {
    return { result: 0, duration: this.duration };
  }

  async getMediaState() {
    return { result: 0, state: this.mediaState };
  }

  async $dispose() {
    await this.closeMedia();
  }
}

// Connect playlist playItem handler to NativeAplayerStack
setPlayItemHandler((item) => {
  if (currentStackInstance) {
    currentStackInstance.openMedia({ name: item.name, playUrl: item.url, mediaType: item.mediaType });
  }
});

/**
 * Register player RPC handlers matching the original Electron server API names.
 *
 * In the original Electron app (xmp_xdas_2), the Vue frontend communicates
 * with the player through `client.callRemoteClientFunction()` using specific
 * API names like 'AplayerStackPlayMedia', 'AplayerStackPauseMedia', etc.
 * These must be registered in `registeredRpcFunctions` with the exact names
 * so the IPC dispatch in net_ipc.ts can find them.
 *
 * State change events are broadcast via `ipcBroadcast('AplayerMeidaPlayStateChange', ...)`
 * which the frontend listens to via `client.attachServerEvent('AplayerMeidaPlayStateChange', ...)`.
 */
export function registerPlayerRpcFunctions() {
  // --- AplayerStack APIs (server/aplayer-stack.ts) ---
  if (!registeredRpcFunctions['AplayerStackPlayMedia']) {
    registeredRpcFunctions['AplayerStackPlayMedia'] = () => {
      const stack = currentStackInstance;
      if (stack) { stack.playMedia(); }
      return 0;
    };
  }
  if (!registeredRpcFunctions['AplayerStackPauseMedia']) {
    registeredRpcFunctions['AplayerStackPauseMedia'] = () => {
      const stack = currentStackInstance;
      if (stack) { stack.pauseMedia(); }
      return 0;
    };
  }
  if (!registeredRpcFunctions['AplayerStackGetCurrPlayMedia']) {
    registeredRpcFunctions['AplayerStackGetCurrPlayMedia'] = () => {
      const stack = currentStackInstance;
      const mediaId = stack ? stack.mediaId : '';
      // Re-broadcast MediaChangeEvent so the frontend calls init() on the
      // AplayerMedia object. Without init(), AttachServerEvent listeners for
      // AplayerMeidaPlayStateChange are never registered and the UI won't
      // update when play/pause state changes.
      if (mediaId) {
        queueMicrotask(() => {
          ipcBroadcast('AplayerStackMediaChangeEvent', mediaId);
        });
      }
      return mediaId;
    };
  }
  if (!registeredRpcFunctions['AplayerStackGetVolume']) {
    registeredRpcFunctions['AplayerStackGetVolume'] = () => {
      return currentVolume;
    };
  }
  if (!registeredRpcFunctions['AplayerStackSetVolume']) {
    registeredRpcFunctions['AplayerStackSetVolume'] = (_ctx: unknown, n: unknown) => {
      if (typeof n === 'number') { currentVolume = n; }
      return 0;
    };
  }
  if (!registeredRpcFunctions['AplayerStackIsSilent']) {
    registeredRpcFunctions['AplayerStackIsSilent'] = () => {
      return currentSilent;
    };
  }
  if (!registeredRpcFunctions['AplayerStackSetSilent']) {
    registeredRpcFunctions['AplayerStackSetSilent'] = (_ctx: unknown, b: unknown) => {
      currentSilent = !!b;
      return 0;
    };
  }
  if (!registeredRpcFunctions['AplayerStackCloseMedia']) {
    registeredRpcFunctions['AplayerStackCloseMedia'] = () => {
      const stack = currentStackInstance;
      if (stack) { stack.closeMedia(); }
      return 0;
    };
  }

  // --- AplayerMedia APIs (server/aplayer-meida.ts) ---
  // The frontend calls these with a media ID; we route to currentStackInstance.
  if (!registeredRpcFunctions['AplayerMediaGetMediaState']) {
    registeredRpcFunctions['AplayerMediaGetMediaState'] = () => {
      const stack = currentStackInstance;
      return stack ? stack.mediaState : -1;
    };
  }
  if (!registeredRpcFunctions['AplayerMediaGetPlayProgress']) {
    registeredRpcFunctions['AplayerMediaGetPlayProgress'] = () => {
      const stack = currentStackInstance;
      return stack ? stack.position : 0;
    };
  }
  if (!registeredRpcFunctions['AplayerMediaGetDuration']) {
    registeredRpcFunctions['AplayerMediaGetDuration'] = () => {
      const stack = currentStackInstance;
      return stack ? stack.duration : 0;
    };
  }
  if (!registeredRpcFunctions['AplayerMediaProgressMoveTo']) {
    registeredRpcFunctions['AplayerMediaProgressMoveTo'] = (_ctx: unknown, _id: unknown, pos: unknown) => {
      const stack = currentStackInstance;
      if (stack && typeof pos === 'number') { stack.setPosition(pos); }
      return 0;
    };
  }
  if (!registeredRpcFunctions['AplayerMediaGetName']) {
    registeredRpcFunctions['AplayerMediaGetName'] = () => '';
  }
  if (!registeredRpcFunctions['AplayerMediaGetAttribute']) {
    registeredRpcFunctions['AplayerMediaGetAttribute'] = () => ({});
  }
  if (!registeredRpcFunctions['AplayerMediaGetType']) {
    registeredRpcFunctions['AplayerMediaGetType'] = () => 0;
  }
  if (!registeredRpcFunctions['AplayerMediaGetMediaErrorInfo']) {
    registeredRpcFunctions['AplayerMediaGetMediaErrorInfo'] = () => ({errCode: 0, errMsg: ''});
  }
}

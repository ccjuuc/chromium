// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* eslint-disable @typescript-eslint/require-await */
/* eslint-disable @typescript-eslint/no-explicit-any */

import {scanDirectoryVideos} from '../require.js';
import {AnyFn, ipcBroadcast, registeredRpcFunctions} from './net_ipc.js';

export interface ActivePlayListItem {
  id: string;
  name: string;
  url: string;
  mediaType: number;
  duration?: number;
}

export const activePlayList: ActivePlayListItem[] = [];
export const activePlayListSelectListeners: Array<(id: string) => void> = [];
export const activePlayListAddListeners: Array<(b: boolean, item?: unknown) => void> = [];
export const activePlayListDeleteListeners: Array<(id: string) => void> = [];
export const activePlayListPreparedListeners: Array<() => void> = [];
export let currentPlayingPlayListId = '';

export function setCurrentPlayingPlayListId(id: string) {
  currentPlayingPlayListId = id;
}

export let onPlayItemRequested: ((item: ActivePlayListItem) => void) | null = null;

export function setPlayItemHandler(handler: (item: ActivePlayListItem) => void) {
  onPlayItemRequested = handler;
}

export const activePlayListManager = {
  getPlayList: () => activePlayList.map(item => ({
    id: item.id,
    name: item.name,
    url: item.url,
    mediaType: item.mediaType,
  })),
  getPlayingItemId: () => currentPlayingPlayListId,
  addLocalItem: (name: string, filePath: string) => {
    const id = 'item_' + Date.now() + '_' + Math.random().toString(36).slice(2, 7);
    const item: ActivePlayListItem = {
      id,
      name: name || filePath.split(/[\\/]/).pop() || 'video.mp4',
      url: filePath,
      mediaType: 5,
    };
    activePlayList.push(item);
    activePlayListAddListeners.forEach(cb => { try { cb(true, item); } catch {} });
    activePlayListPreparedListeners.forEach(cb => { try { cb(); } catch {} });
    ipcBroadcast('AplayerPlayListItemAdd', true, item);
    ipcBroadcast('AplayerPlayListPrepared');
    return id;
  },
  deletePlayListItem: (id: string) => {
    const idx = activePlayList.findIndex(i => i.id === id);
    if (idx >= 0) {
      const removed = activePlayList.splice(idx, 1)[0];
      activePlayListDeleteListeners.forEach(cb => { try { cb(id); } catch {} });
      ipcBroadcast('AplayerPlayListItemDelete', id);
      ipcBroadcast('AplayerPlayListPrepared');
      return removed;
    }
    return null;
  },
  playItem: (id: string) => {
    const item = activePlayList.find(i => i.id === id);
    if (item) {
      currentPlayingPlayListId = id;
      activePlayListSelectListeners.forEach(cb => { try { cb(id); } catch {} });
      ipcBroadcast('AplayerPlayListSelectChange', id);
      if (onPlayItemRequested) {
        onPlayItemRequested(item);
      }
    }
  },
  playNext: () => {
    const idx = activePlayList.findIndex(i => i.id === currentPlayingPlayListId);
    if (idx >= 0 && idx + 1 < activePlayList.length) {
      const nextItem = activePlayList[idx + 1];
      if (nextItem) activePlayListManager.playItem(nextItem.id);
    }
  },
  playPrev: () => {
    const idx = activePlayList.findIndex(i => i.id === currentPlayingPlayListId);
    if (idx > 0) {
      const prevItem = activePlayList[idx - 1];
      if (prevItem) activePlayListManager.playItem(prevItem.id);
    }
  },
  clear: () => {
    activePlayList.length = 0;
    ipcBroadcast('AplayerPlayListPrepared');
  },
  getItemMediaInfo: (id: string, cb?: AnyFn) => {
    const item = activePlayList.find(i => i.id === id);
    const info = {
      duration: (item?.duration) || 60000,
      pos: 0,
      size: 0,
      local: true,
      snapshot: '',
    };
    if (typeof cb === 'function') queueMicrotask(() => cb(info));
    return info;
  },
  isNextLocalPlay: (cb?: AnyFn) => queueMicrotask(() => cb?.(true)),
  isPrevLocalPlay: (cb?: AnyFn) => queueMicrotask(() => cb?.(true)),
  isLocalPlay: (_id: string, cb?: AnyFn) => queueMicrotask(() => cb?.(true)),
  attachPlayListPreparedEvent: (cb: AnyFn) => {
    activePlayListPreparedListeners.push(cb);
    queueMicrotask(() => cb?.());
    return activePlayListPreparedListeners.length;
  },
  attachPlayListSelectChangeEvent: (cb: (id: string) => void) => {
    activePlayListSelectListeners.push(cb);
    return activePlayListSelectListeners.length;
  },
  attachPlayListItemAddEvent: (cb: (b: boolean, item?: unknown) => void) => {
    activePlayListAddListeners.push(cb);
    return activePlayListAddListeners.length;
  },
  attachPlayListItemDeleteEvent: (cb: (id: string) => void) => {
    activePlayListDeleteListeners.push(cb);
    return activePlayListDeleteListeners.length;
  },
  detachPlayListPreparedEvent: () => {},
  detachPlayListSelectChangeEvent: () => {},
  detachPlayListItemAddEvent: () => {},
  detachPlayListItemDeleteEvent: () => {},
};

/**
 * Scan directory for sibling video files to populate playlist automatically
 */
export async function populateSiblingVideos(filePath: string): Promise<void> {
  const isLocalFile = filePath && !filePath.startsWith('http://') && !filePath.startsWith('https://') && !filePath.startsWith('blob:');
  if (!isLocalFile) return;

  try {
    const siblingPaths = await scanDirectoryVideos(filePath);
    if (siblingPaths && siblingPaths.length > 0) {
      activePlayList.length = 0;
      for (const p of siblingPaths) {
        const fileName = p.split(/[\\/]/).pop() || p;
        const id = 'item_' + Date.now() + '_' + Math.random().toString(36).slice(2, 7);
        const item: ActivePlayListItem = {
          id,
          name: fileName,
          url: p,
          mediaType: 5,
        };
        activePlayList.push(item);
        if (p.replace(/\\/g, '/').toLowerCase() === filePath.replace(/\\/g, '/').toLowerCase()) {
          currentPlayingPlayListId = id;
        }
      }
      if (!currentPlayingPlayListId && activePlayList.length > 0) {
        currentPlayingPlayListId = activePlayList[0]?.id || '';
      }
      activePlayListPreparedListeners.forEach(cb => { try { cb(); } catch {} });
      activePlayListSelectListeners.forEach(cb => { try { cb(currentPlayingPlayListId); } catch {} });
      ipcBroadcast('AplayerPlayListPrepared');
      ipcBroadcast('AplayerPlayListSelectChange', currentPlayingPlayListId);
    }
  } catch (err) {
    console.warn('[xenon-player playlist] Failed to scan sibling directory videos:', err);
  }
}

/**
 * Fallback RPC handlers in case Vue UI directly queries AplayerPlayList methods
 */
export function registerPlaylistRpcFallbacks() {
  if (!registeredRpcFunctions['AplayerPlayListGetPlayList']) {
    registeredRpcFunctions['AplayerPlayListGetPlayList'] = async () => activePlayListManager.getPlayList();
  }
  if (!registeredRpcFunctions['AplayerPlayListGetPlayingItemId']) {
    registeredRpcFunctions['AplayerPlayListGetPlayingItemId'] = async () => activePlayListManager.getPlayingItemId();
  }
  if (!registeredRpcFunctions['AplayerPlayListGetItemMediaInfo']) {
    registeredRpcFunctions['AplayerPlayListGetItemMediaInfo'] = async (_ctx: unknown, ...args: unknown[]) => activePlayListManager.getItemMediaInfo(String(args[0] ?? ''));
  }
}

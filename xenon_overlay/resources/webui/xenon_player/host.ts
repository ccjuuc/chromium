// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

import {cleanupElectronShim, installElectronShim} from './electron_shim.js';

const FRONTEND_SCRIPTS = [
  'app/static/js/lib-axios.js',
  'app/static/js/lib-vue.js',
  'app/static/js/881.js',
  'app/static/js/index.js',
];
const DEFAULT_MEDIA_URL = 'i:/gm.mp4';

type BootState = {
  phase: 'loading'|'ready'|'error';
  error?: string;
};

function setBootState(state: BootState) {
  (window as unknown as {__xenonPlayerBootState__: BootState})
      .__xenonPlayerBootState__ = state;
  const status = document.getElementById('boot-status');
  if (!status) {
    return;
  }
  status.dataset['phase'] = state.phase;
  status.textContent = state.phase === 'error' ?
      `Xenon Player failed to start: ${state.error}` :
      'Starting Xenon Player...';
  if (state.phase === 'ready') {
    status.remove();
  }
}

function loadStyle(path: string) {
  const link = document.createElement('link');
  link.rel = 'stylesheet';
  link.href = path;
  document.head.appendChild(link);
}

function loadScript(path: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = path;
    script.onload = () => resolve();
    script.onerror = () => reject(new Error(`Failed to load ${path}`));
    document.head.appendChild(script);
  });
}

async function waitForFrontendMount(timeoutMs = 45000): Promise<void> {
  const startedAt = performance.now();
  while (document.getElementById('root')?.childElementCount === 0) {
    if (performance.now() - startedAt >= timeoutMs) {
      throw new Error('Electron frontend did not mount within the timeout');
    }
    await new Promise(resolve => window.setTimeout(resolve, 50));
  }
}

async function playDefaultMedia(): Promise<void> {
  const playLocalFile =
      (window as unknown as {
        playLocalFile?: (path: string, directUrl?: string) => Promise<unknown>,
      }).playLocalFile;
  if (!playLocalFile) {
    throw new Error('Electron player did not register its media entry point');
  }
  await playLocalFile("", DEFAULT_MEDIA_URL);
}

async function boot() {
  setBootState({phase: 'loading'});
  if (!loadTimeData.getBoolean('hasFrontend')) {
    throw new Error(
        'xenon_player/frontend was not found beside the Xenon executable');
  }

  await installElectronShim(loadTimeData.getString('execPath'));
  loadStyle('app/static/css/index.css');
  for (const script of FRONTEND_SCRIPTS) {
    await loadScript(script);
  }
  await waitForFrontendMount();
  if (new URL(window.location.href).searchParams.get('autoplay') !== '0') {
    await playDefaultMedia();
  }
  setBootState({phase: 'ready'});
}

window.addEventListener('pagehide', () => {
  void cleanupElectronShim();
}, {once: true});

void boot().catch((error: unknown) => {
  const message = error instanceof Error ? error.message : String(error);
  console.error('[xenon-player] startup failed:', error);
  setBootState({phase: 'error', error: message});
});

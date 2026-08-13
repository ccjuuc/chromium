// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {addNodeModuleLoadedListener, getModuleExportTree, inspectExport, isSameModulePath, preparePlayerHost, require as nodeRequire, whenRequired,} from './require.js';
import type {NodeExportInfo} from './require.js';

type NativeResult = unknown;

const DEFAULT_ADDON_PATH = 'test_addon.node';
const PC_ADDON_PATH = 'pc_addon.node';

interface NativeAddon {
  Add(a: number, b: number): Promise<NativeResult>;
  AsyncAdd(
      a: number, b: number,
      callback: (value: NativeResult) => void): Promise<NativeResult>;
  StartThread(callback: (message: NativeResult) => void): Promise<NativeResult>;
  InspectTypes(input: Record<string, unknown>): Promise<NativeResult>;
  BinaryEcho(input: ArrayBuffer|Uint8Array): Promise<NativeResult>;
  PromiseValue(): Promise<NativeResult>;
  MultiCallback(
      first: (value: NativeResult) => void,
      second: (value: NativeResult) => void): Promise<NativeResult>;
  ThrowComplexError(): Promise<NativeResult>;
  UvTimer(callback: (message: NativeResult) => void): Promise<NativeResult>;
}

interface PcAddon {
  initAddon(params: PcAddonInitParams): Promise<NativeResult>;
  getAplayerWnd(callback: (windowHandle: number) => void):
      Promise<NativeResult>;
  setWndEx(floatWindow: number, parentWindow: number): Promise<NativeResult>;
  setPlayableExt(exts: string[]): Promise<NativeResult>;
  NativeAplayerStack?: new(...args: unknown[]) => NativeAplayerStack;
}

interface NativeAplayerStack {
  openMedia(attr: Record<string, unknown>): Promise<NativeResult>;
  $dispose(): Promise<void>;
}

interface PcAddonInitParams {
  xmp: boolean;
  openPlayerLog: boolean;
  logDir: string;
  codecPath: string;
  subtitleCachePath: string;
  peerid: string;
  version: string;
  versionCode: number;
  configPath: string;
  dbDir: string;
  dbName: string;
  panPlayCache: string;
  downloadServerDir: string;
  transmitAppKey: string;
  productName: string;
}

/** Matches pc_addon MediaType. */
const MT_WEB = 3;
const MT_NEW_XMP_LOCAL = 5;

function mediaDisplayName(playUrl: string): string {
  const leaf = playUrl.replace(/[?#].*$/, '').replace(/^.*[/\\]/, '');
  if (!leaf || leaf.startsWith('.')) {
    return 'demo-stream';
  }
  return leaf.replace(/\.[^.]+$/, '') || leaf;
}

declare global {
  interface Window {
    addon?: NativeAddon&Partial<PcAddon>&Record<string, unknown>;
  }
}

function getRequiredElement<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`Missing required element: #${id}`);
  }
  return element as T;
}

function getInputValue(id: string): string {
  return getRequiredElement<HTMLInputElement>(id).value;
}

function setElementText(id: string, text: string) {
  getRequiredElement<HTMLElement>(id).textContent = text;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function formatValue(value: unknown): string {
  if (value instanceof Uint8Array) {
    return `Uint8Array(${value.byteLength}) [${Array.from(value).join(', ')}]`;
  }
  return JSON.stringify(value, (_key, item) => {
    if (item === undefined) {
      return 'undefined';
    }
    if (typeof item === 'bigint') {
      return `${item}n`;
    }
    if (item instanceof Uint8Array) {
      return `Uint8Array(${item.byteLength}) [${Array.from(item).join(', ')}]`;
    }
    if (item instanceof Map) {
      return {type: 'Map', entries: Array.from(item)};
    }
    if (item instanceof Set) {
      return {type: 'Set', values: Array.from(item)};
    }
    if (typeof item === 'number' && !Number.isFinite(item)) {
      return String(item);
    }
    if (Object.is(item, -0)) {
      return '-0';
    }
    return item;
  }, 2) ?? String(value);
}

function addLog(message: string, isError = false) {
  const logList = getRequiredElement<HTMLElement>('log-list');
  const emptyState = logList.querySelector<HTMLElement>('.empty-state');
  if (emptyState) {
    emptyState.remove();
  }

  const logItem = document.createElement('div');
  logItem.className = 'log-item' + (isError ? ' error' : '');

  const timestamp = document.createElement('span');
  timestamp.className = 'log-time';
  timestamp.textContent = new Date().toLocaleTimeString();

  const content = document.createElement('span');
  content.className = 'log-text';
  content.textContent = message;

  logItem.appendChild(timestamp);
  logItem.appendChild(content);
  logList.appendChild(logItem);

  logList.scrollTop = logList.scrollHeight;
}

function isPcAddonRoute(): boolean {
  return window.location.hash.toLowerCase() === '#pc_addon';
}

function exportMembers(info: NodeExportInfo): NodeExportInfo[] {
  const names = new Set(info.children.map(child => child.name));
  return [
    ...info.children,
    ...info.prototype.filter(member => !names.has(member.name)),
  ];
}

function countExportNodes(exportsList: NodeExportInfo[]): number {
  let count = 0;
  for (const info of exportsList) {
    count += 1 + countExportNodes(exportMembers(info));
  }
  return count;
}

function summarizeExports(exportsList: NodeExportInfo[]): string {
  const topLevel = exportsList.length;
  const nested = Math.max(0, countExportNodes(exportsList) - topLevel);
  if (nested === 0) {
    return `${topLevel} 项`;
  }
  return `${topLevel} 项 / 含 ${nested} 个子成员`;
}

function renderExportTree(
    treeHost: HTMLElement, exportsList: NodeExportInfo[],
    options: {expandObjects?: boolean, modulePath?: string} = {}) {
  treeHost.replaceChildren();
  let selectedItem: HTMLElement|null = null;

  const selectItem = (item: HTMLElement) => {
    if (selectedItem) {
      selectedItem.classList.remove('is-selected');
    }
    selectedItem = item;
    selectedItem.classList.add('is-selected');
  };

  const qualifiedName = (parentPath: string, name: string) =>
      parentPath ? `${parentPath}.${name}` : name;

  const buildItems =
      (parent: HTMLElement, infos: NodeExportInfo[], depth: number,
       parentPath: string) => {
        for (const info of infos) {
          const item = document.createElement('li');
          item.className = 'export-item';

          const row = document.createElement('button');
          row.type = 'button';
          row.className = 'export-row';

          const twistie = document.createElement('span');
          twistie.className = 'export-twistie';
          const path = qualifiedName(parentPath, info.name);
          const canInspect = !!options.modulePath &&
              (info.kind === 'object' || info.kind === 'function' ||
               info.kind === 'class');
          let members = exportMembers(info);
          const hasChildren = members.length > 0;
          const expandable = hasChildren || canInspect;
          twistie.textContent = expandable ? '▸' : '•';

          const name = document.createElement('span');
          name.className = 'export-name';
          name.textContent = info.name;

          const kind = document.createElement('span');
          kind.className = `export-kind kind-${info.kind}`;
          kind.textContent = info.kind;

          row.appendChild(twistie);
          row.appendChild(name);
          row.appendChild(kind);
          item.appendChild(row);

          let childList: HTMLUListElement|null = null;
          let inspected = hasChildren;
          const ensureChildList = () => {
            if (!childList) {
              childList = document.createElement('ul');
              childList.className = 'export-children';
              childList.hidden = true;
              item.appendChild(childList);
            }
            return childList;
          };

          if (hasChildren) {
            const list = ensureChildList();
            const shouldExpand = options.expandObjects !== false &&
                (depth === 0 || info.kind === 'object' ||
                 info.kind === 'class');
            list.hidden = !shouldExpand;
            twistie.textContent = list.hidden ? '▸' : '▾';
            buildItems(list, members, depth + 1, path);
          }

          const expand = async (forceOpen?: boolean) => {
            const list = ensureChildList();
            if (!inspected && canInspect && options.modulePath) {
              twistie.textContent = '…';
              try {
                const detailed =
                    await inspectExport(options.modulePath, path);
                info.children = detailed?.children ?? [];
                info.prototype = detailed?.prototype ?? [];
                members = exportMembers(info);
                info.kind = detailed?.kind ?? info.kind;
                kind.className = `export-kind kind-${info.kind}`;
                kind.textContent = info.kind;
                list.replaceChildren();
                if (members.length > 0) {
                  buildItems(list, members, depth + 1, path);
                } else {
                  twistie.textContent = '•';
                  list.remove();
                  childList = null;
                  inspected = true;
                  return;
                }
                inspected = true;
              } catch (err: unknown) {
                twistie.textContent = '▸';
                addLog(`展开导出失败: ${errorMessage(err)}`, true);
                return;
              }
            }
            if (!childList) {
              return;
            }
            if (forceOpen === true) {
              childList.hidden = false;
            } else if (forceOpen === false) {
              childList.hidden = true;
            } else {
              childList.hidden = !childList.hidden;
            }
            twistie.textContent = childList.hidden ? '▸' : '▾';
          };

          row.addEventListener('click', () => {
            selectItem(item);
            if (expandable) {
              void expand();
            }
          });

          if (options.expandObjects !== false && depth === 0 && canInspect &&
              !hasChildren) {
            void expand(true);
          }

          parent.appendChild(item);
        }
      };

  buildItems(treeHost, exportsList, 0, '');
}

function renderExportsPanel(
    panelId: string, treeId: string, countId: string,
    exportsList: NodeExportInfo[]|null, emptyText: string,
    modulePath: string = '') {
  const panel = document.getElementById(panelId);
  const tree = document.getElementById(treeId);
  const count = document.getElementById(countId);
  if (!panel || !tree || !count) {
    return;
  }

  if (!exportsList) {
    panel.hidden = panelId === 'exports-panel';
    count.textContent = emptyText;
    tree.replaceChildren();
    return;
  }

  panel.hidden = false;
  count.textContent = summarizeExports(exportsList);
  renderExportTree(
      tree, exportsList, {expandObjects: true, modulePath});
}

document.addEventListener('DOMContentLoaded', () => {
  addLog('WebUI 模块测试页面初始化完毕');

  const standardTestCard = getRequiredElement<HTMLElement>('card-testing');
  const pcAddonCard = getRequiredElement<HTMLElement>('card-pc-addon');
  let pcAddonAutoLoaded = false;
  let activeAddonPath = '';
  let activeLoadRequestId = 0;
  let initializedPcAddon: Partial<PcAddon>|null = null;
  let pcAddonInitPromise: Promise<void>|null = null;
  let pcAddonPlayerReadyPromise: Promise<void>|null = null;
  let activePlayerStack: NativeAplayerStack|null = null;

  function getPcAddonRuntimeDir(): string {
    const configuredDir = getInputValue('pc-runtime-dir').trim();
    if (configuredDir) {
      return configuredDir;
    }

    const addonPath = getInputValue('addon-path').trim();
    const lastSeparator =
        Math.max(addonPath.lastIndexOf('/'), addonPath.lastIndexOf('\\'));
    return lastSeparator >= 0 ? addonPath.substring(0, lastSeparator) || '.' :
                                '.';
  }

  function createPcAddonInitParams(runtimeDir: string): PcAddonInitParams {
    return {
      xmp: false,
      openPlayerLog: false,
      logDir: runtimeDir,
      codecPath: runtimeDir,
      subtitleCachePath: runtimeDir,
      peerid: '',
      version: '1',
      versionCode: 1,
      configPath: '',
      dbDir: runtimeDir,
      dbName: 'xenon-node-test.db',
      panPlayCache: runtimeDir,
      downloadServerDir: runtimeDir,
      transmitAppKey: 'xenon-node-test',
      productName: 'xenon',
    };
  }

  async function ensurePcAddonInitialized(addon: Partial<PcAddon>):
      Promise<void> {
    if (initializedPcAddon === addon) {
      return;
    }
    const initAddon = addon.initAddon;
    if (typeof initAddon !== 'function') {
      throw new Error('当前模块未导出 initAddon');
    }

    if (!pcAddonInitPromise) {
      const runtimeDir = getPcAddonRuntimeDir();
      const params = createPcAddonInitParams(runtimeDir);
      addLog(`[WebUI] await addon.initAddon(...)，运行目录: ${runtimeDir}`);
      pcAddonInitPromise = initAddon(params).then(() => {
        initializedPcAddon = addon;
        addLog('[pc_addon -> WebUI] initAddon 完成');
      });
    }

    const pendingInit = pcAddonInitPromise;
    try {
      await pendingInit;
    } catch (error: unknown) {
      if (pcAddonInitPromise === pendingInit) {
        pcAddonInitPromise = null;
      }
      throw error;
    }
  }

  async function ensurePcAddonPlayerReady(addon: Partial<PcAddon>):
      Promise<void> {
    const setWndEx = addon.setWndEx;
    const getAplayerWnd = addon.getAplayerWnd;
    if (typeof setWndEx !== 'function' || typeof getAplayerWnd !== 'function') {
      throw new Error('当前模块未导出 setWndEx/getAplayerWnd');
    }
    if (!pcAddonPlayerReadyPromise) {
      pcAddonPlayerReadyPromise = (async () => {
        const handles = await preparePlayerHost();
        addLog(`[WebUI] await addon.setWndEx(${handles.floatWindow}, ${
            handles.parentWindow})`);
        await setWndEx(handles.floatWindow, handles.parentWindow);
        await new Promise<void>((resolve, reject) => {
          let settled = false;
          const timeout = window.setTimeout(() => {
            if (!settled) {
              settled = true;
              reject(new Error('等待播放器窗口超时'));
            }
          }, 15000);

          getAplayerWnd(windowHandle => {
            if (settled) {
              return;
            }
            settled = true;
            window.clearTimeout(timeout);
            if (!Number.isSafeInteger(windowHandle) || windowHandle <= 0) {
              reject(new Error('播放器返回了无效窗口句柄'));
              return;
            }
            addLog(`[pc_addon -> WebUI] getAplayerWnd: ${windowHandle}`);
            resolve();
          }).catch(error => {
            if (settled) {
              return;
            }
            settled = true;
            window.clearTimeout(timeout);
            reject(error);
          });
        });
      })();
    }

    const pending = pcAddonPlayerReadyPromise;
    try {
      await pending;
    } finally {
      if (pcAddonPlayerReadyPromise === pending) {
        pcAddonPlayerReadyPromise = null;
      }
    }
  }

  function getActiveTestCard(): HTMLElement {
    return isPcAddonRoute() ? pcAddonCard : standardTestCard;
  }

  function setLoadStatusLoading(path: string) {
    const statusBox = getRequiredElement<HTMLElement>('load-status');
    const statusDot = statusBox.querySelector<HTMLElement>('.status-dot');
    const statusText = statusBox.querySelector<HTMLElement>('.status-text');
    if (!statusDot || !statusText) {
      throw new Error('Missing load status children');
    }

    statusDot.className = 'status-dot status-offline';
    statusText.textContent = `正在 require: ${path}`;
    getActiveTestCard().classList.add('disabled-card');
    renderExportsPanel(
        'exports-panel', 'exports-tree', 'exports-count', null, '');
  }

  function setLoadStatusSuccess(path: string, exportsList: NodeExportInfo[]) {
    const statusBox = getRequiredElement<HTMLElement>('load-status');
    const statusDot = statusBox.querySelector<HTMLElement>('.status-dot');
    const statusText = statusBox.querySelector<HTMLElement>('.status-text');
    if (!statusDot || !statusText) {
      throw new Error('Missing load status children');
    }

    statusDot.className = 'status-dot status-online';
    statusText.textContent =
        `加载成功: ${path}（${summarizeExports(exportsList)}）`;
    getActiveTestCard().classList.remove('disabled-card');
    renderExportsPanel(
        'exports-panel', 'exports-tree', 'exports-count', exportsList, '',
        path);
  }

  function setLoadStatusFailure(errorMsg: string) {
    const statusBox = getRequiredElement<HTMLElement>('load-status');
    const statusDot = statusBox.querySelector<HTMLElement>('.status-dot');
    const statusText = statusBox.querySelector<HTMLElement>('.status-text');
    if (!statusDot || !statusText) {
      throw new Error('Missing load status children');
    }

    statusDot.className = 'status-dot status-offline';
    statusText.textContent = `加载失败: ${errorMsg}`;
    getActiveTestCard().classList.add('disabled-card');
    renderExportsPanel(
        'exports-panel', 'exports-tree', 'exports-count', null, '');
  }

  async function loadAddon(path: string) {
    if (!path) {
      addLog('请输入有效的插件路径', true);
      return;
    }

    if (activePlayerStack) {
      await activePlayerStack.$dispose().catch(() => {});
      activePlayerStack = null;
    }
    activeAddonPath = path;
    const loadRequestId = ++activeLoadRequestId;
    window.addon = undefined;
    initializedPcAddon = null;
    pcAddonInitPromise = null;
    pcAddonPlayerReadyPromise = null;
    setLoadStatusLoading(path);
    addLog(`[WebUI] const addon = require("${path}")`);
    try {
      const addon = nodeRequire(path) as NativeAddon&Partial<PcAddon>&
          Record<string, unknown>;
      await whenRequired(path);
      if (loadRequestId !== activeLoadRequestId) {
        return;
      }
      window.addon = addon;
      // Cache hits do not re-fire NodeModuleLoaded; refresh UI here.
      setLoadStatusSuccess(path, getModuleExportTree(path) ?? []);
      addLog(`[WebUI] require("${path}") 完成，模块已挂载至 window.addon`);
    } catch (err: unknown) {
      if (loadRequestId !== activeLoadRequestId) {
        return;
      }
      const message = errorMessage(err);
      setLoadStatusFailure(message);
      addLog(`[WebUI] require("${path}") 失败: ${message}`, true);
    }
  }

  function applyRoute() {
    const isPcRoute = isPcAddonRoute();
    getRequiredElement<HTMLInputElement>('addon-path').value =
        isPcRoute ? PC_ADDON_PATH : DEFAULT_ADDON_PATH;
    standardTestCard.hidden = isPcRoute;
    pcAddonCard.hidden = !isPcRoute;

    if (isPcRoute && !pcAddonAutoLoaded) {
      pcAddonAutoLoaded = true;
      void loadAddon(PC_ADDON_PATH);
    }
  }

  addNodeModuleLoadedListener(
      (path: string, success: boolean, errorMsg: string,
       exportsList: NodeExportInfo[]) => {
        // C++ may report an absolute path while the input uses a basename.
        const isActive = isSameModulePath(path, activeAddonPath);
        if (success) {
          const names = exportsList.map(info => info.name);
          addLog(
              `[C++ -> WebUI] 模块 "${path}" 加载成功! 导出接口: [${
                  names.join(', ')}]（${summarizeExports(exportsList)}）`);
          if (isActive) {
            setLoadStatusSuccess(path, exportsList);
          }
          return;
        }

        addLog(`[C++ -> WebUI] 模块加载失败: ${errorMsg}`, true);
        if (isActive) {
          setLoadStatusFailure(errorMsg);
        }
      });

  const btnLoad = document.getElementById('btn-load');
  if (btnLoad) {
    btnLoad.addEventListener('click', async () => {
      const path = getInputValue('addon-path').trim();
      await loadAddon(path);
    });
  }

  const btnSyncCall = document.getElementById('btn-sync-call');
  if (btnSyncCall) {
    btnSyncCall.addEventListener('click', async () => {
      const a = parseInt(getInputValue('sync-a'), 10) || 0;
      const b = parseInt(getInputValue('sync-b'), 10) || 0;
      if (!window.addon) {
        addLog('请先通过 require("*.node") 加载模块', true);
        return;
      }

      addLog(`[WebUI] await addon.Add(${a}, ${b})`);
      try {
        const result = await window.addon.Add(a, b);
        addLog(`[Node Addon -> WebUI] Add 返回结果: ${result}`);
        setElementText('sync-result', `结果: ${result}`);
      } catch (err: unknown) {
        addLog(`Add 调用失败: ${errorMessage(err)}`, true);
      }
    });
  }

  const btnAsyncCall = document.getElementById('btn-async-call');
  if (btnAsyncCall) {
    btnAsyncCall.addEventListener('click', async () => {
      const a = parseInt(getInputValue('async-a'), 10) || 0;
      const b = parseInt(getInputValue('async-b'), 10) || 0;
      if (!window.addon) {
        addLog('请先通过 require("*.node") 加载模块', true);
        return;
      }

      addLog(`[WebUI] await addon.AsyncAdd(${a}, ${b}, callback)`);
      try {
        const result = await window.addon.AsyncAdd(a, b, value => {
          addLog(`[Node Addon callback -> WebUI] AsyncAdd 回调结果: ${value}`);
        });
        setElementText('async-result', `结果: ${result}`);
      } catch (err: unknown) {
        addLog(`AsyncAdd 调用失败: ${errorMessage(err)}`, true);
      }
    });
  }

  const btnThreadStart = document.getElementById('btn-thread-start');
  if (btnThreadStart) {
    btnThreadStart.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("*.node") 加载模块', true);
        return;
      }

      addLog('[WebUI] addon.StartThread(callback)');
      setElementText('thread-result', '状态: 正在运行线程通知...');
      try {
        await window.addon.StartThread(message => {
          addLog(`[Node Addon callback -> WebUI] StartThread 通知: "${message}"`);
          setElementText('thread-result', `最后收到通知: "${message}"`);
        });
      } catch (err: unknown) {
        addLog(`StartThread 调用失败: ${errorMessage(err)}`, true);
      }
    });
  }

  const btnTypesCall = document.getElementById('btn-types-call');
  if (btnTypesCall) {
    btnTypesCall.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("*.node") 加载模块', true);
        return;
      }

      const payload = {
        title: 'xenon structured payload',
        flag: true,
        maybeNull: null,
        maybeUndefined: undefined,
        largeId: 9007199254740993n,
        createdAt: new Date('2026-07-29T00:00:00.000Z'),
        lookup: new Map<string, number>([['alpha', 1], ['beta', 2]]),
        uniqueValues: new Set<unknown>(['xenon', 7, true]),
        specialNumbers: [NaN, Infinity, -Infinity, -0],
        items: [1, 'two', false, {nested: 42}],
      };
      addLog(`[WebUI] await addon.InspectTypes(${formatValue(payload)})`);
      try {
        const result = await window.addon.InspectTypes(payload);
        addLog(`[Node Addon -> WebUI] InspectTypes 返回: ${formatValue(result)}`);
        setElementText('types-result', formatValue(result));
      } catch (err: unknown) {
        addLog(`InspectTypes 调用失败: ${errorMessage(err)}`, true);
      }
    });
  }

  const btnBinaryCall = document.getElementById('btn-binary-call');
  if (btnBinaryCall) {
    btnBinaryCall.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("*.node") 加载模块', true);
        return;
      }

      const bytes = new Uint8Array([1, 3, 5, 7, 9, 11]);
      addLog(`[WebUI] await addon.BinaryEcho(${formatValue(bytes)})`);
      try {
        const result = await window.addon.BinaryEcho(bytes);
        addLog(`[Node Addon -> WebUI] BinaryEcho 返回: ${formatValue(result)}`);
        setElementText('binary-result', formatValue(result));
      } catch (err: unknown) {
        addLog(`BinaryEcho 调用失败: ${errorMessage(err)}`, true);
      }
    });
  }

  const btnPromiseCall = document.getElementById('btn-promise-call');
  if (btnPromiseCall) {
    btnPromiseCall.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("*.node") 加载模块', true);
        return;
      }

      addLog('[WebUI] await addon.PromiseValue()');
      try {
        const result = await window.addon.PromiseValue();
        addLog(`[Node Addon -> WebUI] PromiseValue resolve: ${formatValue(result)}`);
        setElementText('promise-result', formatValue(result));
      } catch (err: unknown) {
        addLog(`PromiseValue 调用失败: ${errorMessage(err)}`, true);
      }
    });
  }

  const btnMultiCallbackCall =
      document.getElementById('btn-multi-callback-call');
  if (btnMultiCallbackCall) {
    btnMultiCallbackCall.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("*.node") 加载模块', true);
        return;
      }

      addLog('[WebUI] await addon.MultiCallback(firstCallback, secondCallback)');
      try {
        const result = await window.addon.MultiCallback(
            value => {
              addLog(`[Node Addon callback -> WebUI] first: ${formatValue(value)}`);
            },
            value => {
              addLog(`[Node Addon callback -> WebUI] second: ${formatValue(value)}`);
            });
        addLog(`[Node Addon -> WebUI] MultiCallback 返回: ${formatValue(result)}`);
        setElementText('multi-callback-result', formatValue(result));
      } catch (err: unknown) {
        addLog(`MultiCallback 调用失败: ${errorMessage(err)}`, true);
      }
    });
  }

  const btnErrorCall = document.getElementById('btn-error-call');
  if (btnErrorCall) {
    btnErrorCall.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("*.node") 加载模块', true);
        return;
      }

      addLog('[WebUI] await addon.ThrowComplexError()');
      try {
        await window.addon.ThrowComplexError();
        addLog('ThrowComplexError 未抛出异常', true);
      } catch (err: unknown) {
        const message = errorMessage(err);
        addLog(`[Node Addon -> WebUI] ThrowComplexError 捕获异常: ${message}`);
        setElementText('error-result', message);
      }
    });
  }

  const btnUvTimerCall = document.getElementById('btn-uv-timer-call');
  if (btnUvTimerCall) {
    btnUvTimerCall.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("test_addon.node") 加载模块', true);
        return;
      }
      if (typeof window.addon.UvTimer !== 'function') {
        addLog('当前模块未导出 UvTimer', true);
        return;
      }

      setElementText('uv-timer-result', '等待 libuv 回调...');
      addLog('[WebUI] await addon.UvTimer(callback)');
      try {
        await window.addon.UvTimer(message => {
          addLog(`[libuv timer -> WebUI] ${formatValue(message)}`);
          setElementText('uv-timer-result', formatValue(message));
        });
      } catch (err: unknown) {
        const message = errorMessage(err);
        addLog(`UvTimer 调用失败: ${message}`, true);
        setElementText('uv-timer-result', `错误: ${message}`);
      }
    });
  }

  const btnPcSetPlayableExt =
      document.getElementById('btn-pc-set-playable-ext');
  if (btnPcSetPlayableExt) {
    btnPcSetPlayableExt.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("pc_addon.node") 加载模块', true);
        return;
      }
      if (typeof window.addon.setPlayableExt !== 'function') {
        addLog('当前模块未导出 setPlayableExt', true);
        return;
      }

      const exts = ['mp4', 'mkv'];
      addLog(`[WebUI] await addon.setPlayableExt(${formatValue(exts)})`);
      try {
        const result = await window.addon.setPlayableExt(exts);
        addLog(`[pc_addon -> WebUI] setPlayableExt 返回: ${formatValue(result)}`);
        setElementText('pc-set-playable-ext-result', formatValue(result));
      } catch (err: unknown) {
        addLog(`setPlayableExt 调用失败: ${errorMessage(err)}`, true);
      }
    });
  }

  const btnPcInitAddon = document.getElementById('btn-pc-init-addon');
  if (btnPcInitAddon) {
    btnPcInitAddon.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("pc_addon.node") 加载模块', true);
        return;
      }

      try {
        await ensurePcAddonInitialized(window.addon);
        setElementText('pc-init-addon-result', '初始化成功');
      } catch (err: unknown) {
        const message = errorMessage(err);
        addLog(`initAddon 调用失败: ${message}`, true);
        setElementText('pc-init-addon-result', `错误: ${message}`);
      }
    });
  }

  const btnPcOpenMedia = document.getElementById('btn-pc-open-media');
  if (btnPcOpenMedia) {
    btnPcOpenMedia.addEventListener('click', async () => {
      if (!window.addon) {
        addLog('请先通过 require("pc_addon.node") 加载模块', true);
        return;
      }

      const Ctor = window.addon.NativeAplayerStack;
      if (typeof Ctor !== 'function') {
        addLog('当前模块未导出 NativeAplayerStack 类', true);
        return;
      }

      const playUrl = getInputValue('pc-open-media-url').trim();
      if (!playUrl) {
        addLog('请输入 playUrl（本地路径或 http(s) 地址）', true);
        return;
      }

      const isRemoteUrl = /^https?:\/\//i.test(playUrl);
      const attr = {
        name: mediaDisplayName(playUrl),
        gcid: '',
        playUrl,
        playFrom: 'xenon-node-test',
        zipPlay: 0,
        dlnaPlay: 0,
        mediaType: isRemoteUrl ? MT_WEB : MT_NEW_XMP_LOCAL,
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

      try {
        await ensurePcAddonInitialized(window.addon);
        await ensurePcAddonPlayerReady(window.addon);
        if (activePlayerStack) {
          await activePlayerStack.$dispose();
          activePlayerStack = null;
        }
        addLog('[WebUI] const stack = new addon.NativeAplayerStack()');
        const stack = new Ctor();
        try {
          addLog(`[WebUI] await stack.openMedia(${formatValue(attr)})`);
          const result = await stack.openMedia(attr);
          activePlayerStack = stack;
          addLog(`[pc_addon -> WebUI] openMedia 返回: ${formatValue(result)}`);
          setElementText('pc-open-media-result', formatValue(result));
        } catch (error) {
          await stack.$dispose().catch(() => {});
          throw error;
        }
      } catch (err: unknown) {
        const message = errorMessage(err);
        addLog(`openMedia 调用失败: ${message}`, true);
        setElementText('pc-open-media-result', `错误: ${message}`);
      }
    });
  }

  const btnClearLogs = document.getElementById('btn-clear-logs');
  if (btnClearLogs) {
    btnClearLogs.addEventListener('click', () => {
      getRequiredElement<HTMLElement>('log-list').innerHTML =
          '<div class="log-item empty-state">暂无日志，加载模块后开始测试</div>';
    });
  }

  window.addEventListener('hashchange', applyRoute);
  applyRoute();
});

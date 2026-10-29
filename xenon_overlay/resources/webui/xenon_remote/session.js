// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Shared by the privileged host and the portable, unprivileged viewer. Keep
// this a classic script so the viewer also works when opened from a local file.
(() => {
  'use strict';

  const PROTOCOL = 'xenon-remote';
  const VERSION = 1;
  const PAIRING_PREFIX = 'XENON-REMOTE-1.';
  const MAX_PAIRING_LENGTH = 262144;
  const MAX_MESSAGE_LENGTH = 16384;
  const MAX_BUFFERED_AMOUNT = 65536;
  const MODES = Object.freeze(['tab', 'window', 'screen']);

  function isRecord(value) {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
  }

  function validGeneration(value) {
    return Number.isSafeInteger(value) && value > 0 && value <= 2147483647;
  }

  function validatePairing(value, expectedType) {
    if (!isRecord(value) || value.protocol !== PROTOCOL ||
        value.version !== VERSION || value.type !== expectedType ||
        !['offer', 'answer'].includes(expectedType) ||
        typeof value.session !== 'string' ||
        !/^[a-f0-9]{32}$/.test(value.session) ||
        typeof value.sdp !== 'string' || value.sdp.length > 180000 ||
        !value.sdp.startsWith('v=0') ||
        !value.sdp.includes('a=fingerprint:')) {
      throw new Error('配对信息无效，或来自不兼容的版本。');
    }
    return {protocol: PROTOCOL, version: VERSION, type: expectedType,
      session: value.session, sdp: value.sdp};
  }

  function encodePairing(description, session) {
    const value = validatePairing({protocol: PROTOCOL, version: VERSION,
      type: description.type, sdp: description.sdp, session}, description.type);
    const bytes = new TextEncoder().encode(JSON.stringify(value));
    let binary = '';
    for (const byte of bytes) binary += String.fromCharCode(byte);
    const encoded = PAIRING_PREFIX + btoa(binary);
    if (encoded.length > MAX_PAIRING_LENGTH) {
      throw new Error('配对信息过长，请减少网络接口或 ICE 服务器后重试。');
    }
    return encoded;
  }

  function decodePairing(text, expectedType) {
    if (typeof text !== 'string' || text.length > MAX_PAIRING_LENGTH) {
      throw new Error('配对信息过长。');
    }
    const input = text.trim();
    if (!input.startsWith(PAIRING_PREFIX)) {
      throw new Error('请粘贴完整的 Xenon 配对信息。');
    }
    try {
      const binary = atob(input.slice(PAIRING_PREFIX.length));
      const bytes = Uint8Array.from(binary, c => c.charCodeAt(0));
      const decoded = new TextDecoder('utf-8', {fatal: true}).decode(bytes);
      return validatePairing(JSON.parse(decoded), expectedType);
    } catch (error) {
      throw new Error('配对信息无效，或来自不兼容的版本。');
    }
  }

  function parseIceServers(text) {
    if (!text.trim()) return [];
    let servers;
    try {
      if (text.length > MAX_MESSAGE_LENGTH) throw new Error();
      servers = JSON.parse(text);
    } catch {
      throw new Error('ICE 服务器配置必须是 JSON 数组。');
    }
    if (!Array.isArray(servers) || servers.length > 8) {
      throw new Error('最多配置 8 个 ICE 服务器。');
    }
    return servers.map(server => {
      if (!isRecord(server)) throw new Error('ICE 服务器格式无效。');
      const urls = Array.isArray(server.urls) ? server.urls : [server.urls];
      if (!urls.length || urls.length > 8 || urls.some(url =>
        typeof url !== 'string' || url.length > 2048 ||
        !/^(stun|stuns|turn|turns):[^\s]+$/.test(url))) {
        throw new Error('ICE 地址必须以 stun:、stuns:、turn: 或 turns: 开头。');
      }
      const result = {urls};
      for (const key of ['username', 'credential']) {
        if (server[key] !== undefined) {
          if (typeof server[key] !== 'string' || server[key].length > 1024) {
            throw new Error('ICE 用户名或凭据格式无效。');
          }
          result[key] = server[key];
        }
      }
      return result;
    });
  }

  function validateSource(value) {
    if (!isRecord(value) || !validGeneration(value.generation) ||
        !MODES.includes(value.mode) || typeof value.control !== 'boolean' ||
        typeof value.active !== 'boolean' ||
        !Number.isSafeInteger(value.revision) || value.revision < 0) {
      return null;
    }
    return {generation: value.generation, mode: value.mode,
      control: value.active && value.control, active: value.active,
      revision: value.revision};
  }

  function validateInput(value) {
    if (!isRecord(value) || !validGeneration(value.generation)) return null;
    const input = {generation: value.generation, type: value.type};
    if (value.type === 'release') return input;
    if (['move', 'button', 'wheel'].includes(value.type)) {
      const modifiers = value.modifiers === undefined ? 0 : value.modifiers;
      if (![value.x, value.y].every(n =>
        Number.isFinite(n) && n >= 0 && n <= 1) ||
          !Number.isInteger(modifiers) || modifiers < 0 || modifiers > 15) return null;
      input.x = value.x;
      input.y = value.y;
      input.modifiers = modifiers;
      if (value.type === 'button') {
        if (![0, 1, 2].includes(value.button) ||
            typeof value.down !== 'boolean') return null;
        input.button = value.button;
        input.down = value.down;
      }
      if (value.type === 'wheel') {
        if (![value.deltaX, value.deltaY].every(n =>
          Number.isFinite(n) && Math.abs(n) <= 10000)) return null;
        input.deltaX = value.deltaX;
        input.deltaY = value.deltaY;
      }
      return input;
    }
    if (value.type === 'key') {
      if (typeof value.code !== 'string' || !value.code ||
          value.code.length > 64 || typeof value.key !== 'string' ||
          value.key.length > 64 || typeof value.down !== 'boolean' ||
          !Number.isInteger(value.modifiers) || value.modifiers < 0 ||
          value.modifiers > 15) return null;
      return {...input, code: value.code, key: value.key, down: value.down,
        modifiers: value.modifiers, repeat: value.repeat === true};
    }
    if (value.type === 'text') {
      if (typeof value.text !== 'string' || !value.text ||
          value.text.length > 4096 || value.text.includes('\0') ||
          new TextEncoder().encode(value.text).length > 4096) return null;
      return {...input, text: value.text};
    }
    return null;
  }

  function parseMessage(data) {
    if (typeof data !== 'string' || data.length > MAX_MESSAGE_LENGTH ||
        new TextEncoder().encode(data).length > MAX_MESSAGE_LENGTH) return null;
    try {
      const message = JSON.parse(data);
      if (!isRecord(message) || message.protocol !== PROTOCOL ||
          message.version !== VERSION) return null;
      if (message.type === 'source') {
        const source = validateSource(message.source);
        return source ? {type: 'source', source} : null;
      }
      if (message.type === 'input') {
        const input = validateInput(message.input);
        return input ? {type: 'input', input} : null;
      }
      return null;
    } catch {
      return null;
    }
  }

  function normalizedPoint(rect, videoWidth, videoHeight, clientX, clientY,
                           clampOutside = false) {
    if (![rect.width, rect.height, videoWidth, videoHeight].every(n =>
      Number.isFinite(n) && n > 0) ||
      ![rect.left, rect.top, clientX, clientY].every(Number.isFinite)) return null;
    const scale = Math.min(rect.width / videoWidth, rect.height / videoHeight);
    const width = videoWidth * scale;
    const height = videoHeight * scale;
    const x = (clientX - rect.left - (rect.width - width) / 2) / width;
    const y = (clientY - rect.top - (rect.height - height) / 2) / height;
    if (!clampOutside && (x < 0 || x > 1 || y < 0 || y > 1)) return null;
    return {x: Math.min(1, Math.max(0, x)), y: Math.min(1, Math.max(0, y))};
  }

  function modifiersFor(event) {
    return (event.altKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) |
      (event.metaKey ? 4 : 0) | (event.shiftKey ? 8 : 0);
  }

  function stopStream(stream) {
    for (const track of stream?.getTracks() || []) track.stop();
  }

  class OperationEpoch {
    value = 0;
    next() { return ++this.value; }
    isCurrent(value) { return this.value === value; }
  }

  function randomSessionId() {
    return Array.from(crypto.getRandomValues(new Uint8Array(16)),
      byte => byte.toString(16).padStart(2, '0')).join('');
  }

  function waitForIce(peer, timeoutMs = 20000) {
    if (peer.iceGatheringState === 'complete') return Promise.resolve();
    return new Promise((resolve, reject) => {
      const finish = error => {
        clearTimeout(timer);
        peer.removeEventListener('icegatheringstatechange', check);
        peer.removeEventListener('connectionstatechange', check);
        error ? reject(error) : resolve();
      };
      const check = () => {
        if (peer.connectionState === 'closed') {
          finish(new Error('连接已取消。'));
        } else if (peer.iceGatheringState === 'complete') {
          finish();
        }
      };
      const timer = setTimeout(() => finish(new Error(
        '网络地址收集超时。请检查 ICE 服务器配置，然后重新配对。')), timeoutMs);
      peer.addEventListener('icegatheringstatechange', check);
      peer.addEventListener('connectionstatechange', check);
      check();
    });
  }

  // Keep the newest pointer position per displayed frame. Discrete input must
  // flush first; release/revocation must cancel so an old move cannot arrive
  // after a click, a key transition or a change of capture source.
  class PointerMoveQueue {
    constructor(send, schedule = callback => requestAnimationFrame(callback),
      cancel = id => cancelAnimationFrame(id)) {
      this.send = send;
      this.schedule = schedule;
      this.cancelScheduled = cancel;
      this.pending = null;
      this.frame = null;
    }

    push(input) {
      this.pending = {...input};
      if (this.frame === null) {
        this.frame = this.schedule(() => {
          this.frame = null;
          this.flush();
        });
      }
    }

    flush() {
      const input = this.pending;
      this.cancel();
      if (input) this.send(input);
    }

    cancel() {
      if (this.frame !== null) this.cancelScheduled(this.frame);
      this.frame = null;
      this.pending = null;
    }
  }

  class RemoteSession {
    constructor(role, callbacks = {}, peerFactory = config =>
      new RTCPeerConnection(config)) {
      if (!['host', 'viewer'].includes(role)) throw new Error('Invalid role');
      this.role = role;
      this.callbacks = callbacks;
      this.peerFactory = peerFactory;
      this.epoch = new OperationEpoch();
      this.peer = null;
      this.channel = null;
      this.senders = {};
      this.sessionId = '';
      this.source = null;
      this.pendingSource = false;
      this.pendingRelease = false;
      this.remoteStream = null;
    }

    notify(name, ...args) { this.callbacks[name]?.(...args); }

    open(iceServers) {
      const epoch = this.epoch.next();
      const peer = this.peerFactory({iceServers});
      this.peer = peer;
      peer.onconnectionstatechange = () => {
        if (!this.epoch.isCurrent(epoch)) return;
        this.notify('onState', peer.connectionState);
        if (['disconnected', 'failed', 'closed'].includes(peer.connectionState)) {
          // Control never survives a transport interruption without a fresh
          // local grant, even if ICE later recovers.
          if (this.source) this.source.control = false;
          this.notify('onInterrupted', peer.connectionState);
        }
      };
      peer.ontrack = event => {
        if (!this.epoch.isCurrent(epoch)) return;
        if (!this.remoteStream) this.remoteStream = new MediaStream();
        if (!this.remoteStream.getTracks().includes(event.track)) {
          this.remoteStream.addTrack(event.track);
        }
        this.notify('onStream', this.remoteStream);
      };
      peer.ondatachannel = event => {
        if (!this.epoch.isCurrent(epoch) || this.role !== 'viewer' ||
            event.channel.label !== PROTOCOL || this.channel) {
          event.channel.close();
          return;
        }
        this.attachChannel(event.channel, epoch);
      };
      return {peer, epoch};
    }

    attachChannel(channel, epoch) {
      this.channel = channel;
      channel.bufferedAmountLowThreshold = MAX_BUFFERED_AMOUNT / 2;
      channel.onopen = () => {
        if (!this.epoch.isCurrent(epoch)) return;
        this.flush();
        this.notify('onChannel', true);
      };
      channel.onbufferedamountlow = () => {
        if (this.epoch.isCurrent(epoch)) this.flush();
      };
      channel.onclose = () => {
        if (!this.epoch.isCurrent(epoch)) return;
        if (this.source) this.source.control = false;
        this.notify('onChannel', false);
        this.notify('onInterrupted', 'channel-closed');
      };
      channel.onerror = () => {
        if (!this.epoch.isCurrent(epoch)) return;
        if (this.source) this.source.control = false;
        this.notify('onInterrupted', 'channel-error');
      };
      channel.onmessage = event => {
        if (!this.epoch.isCurrent(epoch)) return;
        const message = parseMessage(event.data);
        if (!message) return;
        if (this.role === 'viewer' && message.type === 'source') {
          const next = message.source;
          if (this.source && (next.generation < this.source.generation ||
              (next.generation === this.source.generation &&
               next.revision <= this.source.revision))) return;
          this.source = next;
          this.pendingRelease = false;
          this.notify('onSource', {...next});
        } else if (this.role === 'host' && message.type === 'input' &&
            this.source?.active &&
            message.input.generation === this.source.generation &&
            (this.source.control || message.input.type === 'release')) {
          this.notify('onInput', message.input);
        }
      };
    }

    send(type, body) {
      if (this.channel?.readyState !== 'open' ||
          this.channel.bufferedAmount > MAX_BUFFERED_AMOUNT) return false;
      const data = JSON.stringify({protocol: PROTOCOL, version: VERSION, type,
        [type]: body});
      if (data.length > MAX_MESSAGE_LENGTH) return false;
      try {
        this.channel.send(data);
        return true;
      } catch {
        return false;
      }
    }

    flush() {
      if (this.role === 'host' && this.pendingSource && this.source) {
        this.pendingSource = !this.send('source', this.source);
      }
      if (this.role === 'viewer' && this.pendingRelease && this.source) {
        this.pendingRelease = !this.send('input', {
          type: 'release', generation: this.source.generation});
      }
    }

    setSource(source) {
      if (this.role !== 'host') return;
      const next = validateSource({...source, revision:
        this.source?.generation === source.generation ?
          this.source.revision + 1 : 0});
      if (!next) throw new Error('Invalid capture source');
      this.source = next;
      this.pendingSource = true;
      this.flush();
    }

    sendInput(input) {
      if (this.role !== 'viewer' || !this.source?.active ||
          (input.type !== 'release' && !this.source.control)) return false;
      const value = validateInput({...input, generation: this.source.generation});
      if (!value) return false;
      // A deferred release must precede new input even if the buffer has not
      // yet crossed bufferedAmountLowThreshold. Otherwise it can release a
      // newer key/button after that input has already reached the host.
      if (this.pendingRelease) {
        this.flush();
        if (this.pendingRelease) return false;
        if (value.type === 'release') return true;
      }
      const sent = this.send('input', value);
      if (!sent && (value.type === 'release' ||
          (['key', 'button'].includes(value.type) && !value.down))) {
        this.pendingRelease = true;
      }
      return sent;
    }

    async createOffer(stream, iceServers = []) {
      if (this.role !== 'host' || !stream?.getVideoTracks().length) {
        throw new Error('请先选择共享内容。');
      }
      this.close();
      const {peer, epoch} = this.open(iceServers);
      this.sessionId = randomSessionId();
      this.senders.video = peer.addTransceiver('video',
        {direction: 'sendonly'}).sender;
      this.senders.audio = peer.addTransceiver('audio',
        {direction: 'sendonly'}).sender;
      this.attachChannel(peer.createDataChannel(PROTOCOL, {ordered: true}), epoch);
      await this.replaceStream(stream);
      if (!this.epoch.isCurrent(epoch)) throw new Error('配对已取消。');
      await peer.setLocalDescription(await peer.createOffer());
      await waitForIce(peer);
      if (!this.epoch.isCurrent(epoch)) throw new Error('配对已取消。');
      return encodePairing(peer.localDescription, this.sessionId);
    }

    async createAnswer(offerText, iceServers = []) {
      const offer = decodePairing(offerText, 'offer');
      if (this.role !== 'viewer') throw new Error('Invalid role');
      this.close();
      const {peer, epoch} = this.open(iceServers);
      this.sessionId = offer.session;
      await peer.setRemoteDescription({type: 'offer', sdp: offer.sdp});
      if (!this.epoch.isCurrent(epoch)) throw new Error('配对已取消。');
      await peer.setLocalDescription(await peer.createAnswer());
      await waitForIce(peer);
      if (!this.epoch.isCurrent(epoch)) throw new Error('配对已取消。');
      return encodePairing(peer.localDescription, this.sessionId);
    }

    async acceptAnswer(answerText) {
      const answer = decodePairing(answerText, 'answer');
      if (this.role !== 'host' || !this.peer ||
          this.peer.signalingState !== 'have-local-offer' ||
          answer.session !== this.sessionId) {
        throw new Error('回复不属于当前配对，请使用最新生成的配对信息。');
      }
      await this.peer.setRemoteDescription({type: 'answer', sdp: answer.sdp});
    }

    async replaceStream(stream) {
      if (this.role !== 'host' || !this.peer) return;
      const peer = this.peer;
      const videoTrack = stream?.getVideoTracks()[0] || null;
      const audioTrack = stream?.getAudioTracks()[0] || null;
      await Promise.all([
        this.senders.video?.replaceTrack(videoTrack),
        this.senders.audio?.replaceTrack(audioTrack),
      ]);
      if (this.peer !== peer) throw new Error('连接已更换。');
    }

    close() {
      this.epoch.next();
      const channel = this.channel;
      const peer = this.peer;
      this.channel = null;
      this.peer = null;
      this.senders = {};
      this.sessionId = '';
      this.pendingRelease = false;
      this.source = null;
      this.pendingSource = false;
      channel?.close();
      peer?.close();
      stopStream(this.remoteStream);
      this.remoteStream = null;
    }
  }

  globalThis.XenonRemote = Object.freeze({PROTOCOL, VERSION,
    MAX_PAIRING_LENGTH, MAX_MESSAGE_LENGTH, MAX_BUFFERED_AMOUNT, MODES,
    encodePairing, decodePairing, parseIceServers, validateInput, validateSource,
    parseMessage, normalizedPoint, modifiersFor, stopStream, OperationEpoch,
    PointerMoveQueue, RemoteSession, waitForIce});
})();

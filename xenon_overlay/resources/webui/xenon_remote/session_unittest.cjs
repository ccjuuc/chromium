// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {webcrypto} = require('node:crypto');
const test = require('node:test');

const context = vm.createContext({TextEncoder, TextDecoder, Uint8Array,
  btoa, atob, crypto: webcrypto, setTimeout, clearTimeout});
vm.runInContext(fs.readFileSync(path.join(__dirname, 'session.js'), 'utf8'),
  context, {filename: 'session.js'});
const remote = context.XenonRemote;
const clean = value => JSON.parse(JSON.stringify(value));
const sessionId = '0123456789abcdef0123456789abcdef';
const sdp = 'v=0\r\na=fingerprint:sha-256 AA:BB\r\n';
const description = type => ({type, sdp});
const source = (generation = 1, control = false) =>
  ({generation, mode: 'tab', active: true, control});
const wire = (type, value) => JSON.stringify({protocol: remote.PROTOCOL,
  version: remote.VERSION, type, [type]: value});
const key = (down = true, generation = 1) =>
  ({type: 'key', generation, code: 'KeyA', key: 'a', down, modifiers: 0});

class FakeChannel {
  label = remote.PROTOCOL;
  readyState = 'connecting';
  bufferedAmount = 0;
  sent = [];
  send(data) {
    if (this.readyState !== 'open') throw new Error('closed');
    this.sent.push(JSON.parse(data));
  }
  open() { this.readyState = 'open'; this.onopen?.(); }
  close() { this.readyState = 'closed'; this.onclose?.(); }
  receive(data) { this.onmessage?.({data}); }
}

class FakePeer extends EventTarget {
  connectionState = 'new';
  iceGatheringState = 'complete';
  signalingState = 'stable';
  transceivers = [];
  constructor(config) { super(); this.config = config; }
  createDataChannel() { return this.channel = new FakeChannel(); }
  addTransceiver(kind, options) {
    const sender = {track: null, replaced: [], async replaceTrack(track) {
      this.track = track;
      this.replaced.push(track);
    }};
    const transceiver = {kind, options, sender};
    this.transceivers.push(transceiver);
    return transceiver;
  }
  async createOffer() { return description('offer'); }
  async createAnswer() { return description('answer'); }
  async setLocalDescription(value) {
    if (this.connectionState === 'closed') throw new Error('closed');
    this.localDescription = value;
    this.signalingState = value.type === 'offer' ? 'have-local-offer' : 'stable';
  }
  async setRemoteDescription(value) {
    this.remoteDescription = value;
    this.signalingState = value.type === 'offer' ? 'have-remote-offer' : 'stable';
  }
  state(state) {
    this.connectionState = state;
    this.onconnectionstatechange?.();
    this.dispatchEvent(new Event('connectionstatechange'));
  }
  close() { this.state('closed'); }
}

function stream(video = {id: 'video'}, audio = null) {
  return {getVideoTracks: () => video ? [video] : [],
    getAudioTracks: () => audio ? [audio] : [],
    getTracks: () => [video, audio].filter(Boolean)};
}

function fixture(role, callbacks = {}) {
  const peers = [];
  const session = new remote.RemoteSession(role, callbacks, config => {
    const peer = new FakePeer(config);
    peers.push(peer);
    return peer;
  });
  return {session, peers};
}

function pointerFixture() {
  const callbacks = new Map();
  const sent = [];
  let next = 0;
  const queue = new remote.PointerMoveQueue(input => sent.push(clean(input)),
    callback => { callbacks.set(++next, callback); return next; },
    id => callbacks.delete(id));
  return {queue, callbacks, sent, frame() {
    const pending = [...callbacks.values()];
    callbacks.clear();
    pending.forEach(callback => callback());
  }};
}

test('high-rate pointer moves send only the latest position per animation frame', () => {
  const f = pointerFixture();
  for (let i = 0; i < 100; ++i) {
    f.queue.push({type: 'move', x: i / 100, y: .5, modifiers: 8});
  }
  assert.equal(f.callbacks.size, 1);
  assert.equal(f.sent.length, 0);
  f.frame();
  assert.deepEqual(f.sent, [{type: 'move', x: .99, y: .5, modifiers: 8}]);
});

test('click and key barriers flush movement without a late duplicate', () => {
  const f = pointerFixture();
  f.queue.push({type: 'move', x: .2, y: .3});
  f.queue.flush();
  f.sent.push({type: 'button', down: true});
  f.queue.push({type: 'move', x: .8, y: .3});
  f.queue.flush();
  f.sent.push({type: 'key', down: false});
  f.frame();
  assert.deepEqual(f.sent.map(input => input.type), ['move', 'button', 'move', 'key']);
  assert.equal(f.sent[2].x, .8);
});

test('release or source change discards pending movement before the next grant', () => {
  const f = pointerFixture();
  f.queue.push({type: 'move', generation: 1, x: .1, y: .1});
  f.queue.cancel();
  f.frame();
  assert.equal(f.sent.length, 0);
  f.queue.push({type: 'move', generation: 2, x: .9, y: .9});
  f.frame();
  assert.deepEqual(f.sent, [{type: 'move', generation: 2, x: .9, y: .9}]);
});

test('pairing round-trips SDP and binds type, version and session identity', () => {
  for (const type of ['offer', 'answer']) {
    const encoded = remote.encodePairing(description(type), sessionId);
    assert.deepEqual(clean(remote.decodePairing(` ${encoded}\n`, type)), {
      protocol: 'xenon-remote', version: 1, type, sdp, session: sessionId,
    });
    assert.throws(() => remote.decodePairing(encoded,
      type === 'offer' ? 'answer' : 'offer'), /配对信息无效/);
  }
  const malformed = value => 'XENON-REMOTE-1.' + btoa(JSON.stringify(value));
  const valid = {protocol: 'xenon-remote', version: 1,
    type: 'offer', sdp, session: sessionId};
  for (const value of [{...valid, version: 2}, {...valid, session: 'guessable'},
    {...valid, sdp: 'not SDP'}, {...valid, protocol: 'different-app'}]) {
    assert.throws(() => remote.decodePairing(malformed(value), 'offer'));
  }
  assert.throws(() => remote.decodePairing('XENON-REMOTE-1.%%%', 'offer'));
  assert.throws(() => remote.decodePairing('x'.repeat(262145), 'offer'), /过长/);
});

test('ICE configuration accepts only bounded STUN/TURN server configuration', () => {
  assert.deepEqual(clean(remote.parseIceServers('')), []);
  assert.deepEqual(clean(remote.parseIceServers(JSON.stringify([
    {urls: 'stun:example.test:3478', ignored: true},
    {urls: ['turn:example.test:3478?transport=udp'], username: 'u', credential: 'p'},
  ]))), [{urls: ['stun:example.test:3478']},
    {urls: ['turn:example.test:3478?transport=udp'], username: 'u', credential: 'p'}]);
  for (const text of ['{}', '[{"urls":"https://example.test"}]',
    '[{"urls":[]}]', '[{"urls":"turn:example.test","credential":12}]',
    JSON.stringify(Array(9).fill({urls: 'stun:example.test'}))]) {
    assert.throws(() => remote.parseIceServers(text));
  }
});

test('pointer coordinates exclude letterboxing and clamp captured drags', () => {
  const rect = {left: 10, top: 20, width: 1000, height: 1000};
  assert.deepEqual(clean(remote.normalizedPoint(rect, 1920, 1080, 510, 520)),
    {x: .5, y: .5});
  assert.equal(remote.normalizedPoint(rect, 1920, 1080, 510, 21), null);
  assert.deepEqual(clean(remote.normalizedPoint(rect, 1920, 1080, -20, 21, true)),
    {x: 0, y: 0});
  assert.equal(remote.normalizedPoint(rect, 0, 1080, 510, 520), null);
  assert.equal(remote.normalizedPoint(rect, 1920, 1080, NaN, 520), null);
});

test('input validation rejects malformed coordinates, oversized text and arbitrary commands', () => {
  assert.deepEqual(clean(remote.validateInput(key())), {...key(), repeat: false});
  assert.equal(remote.validateInput({...key(), modifiers: 16}), null);
  assert.equal(remote.validateInput({...key(), generation: -1}), null);
  assert.equal(remote.validateInput({generation: 1, type: 'move', x: NaN, y: 0}), null);
  assert.equal(remote.validateInput({generation: 1, type: 'move', x: -1, y: 0}), null);
  assert.equal(remote.validateInput({generation: 1, type: 'button',
    x: .5, y: .5, button: 5, down: true}), null);
  assert.equal(remote.validateInput({generation: 1, type: 'text',
    text: '中'.repeat(1366)}), null);
  assert.equal(remote.validateInput({generation: 1, type: 'text', text: 'a\0b'}), null);
  assert.equal(remote.validateInput({generation: 1, type: 'Runtime.evaluate',
    expression: 'danger()'}), null);
  assert.equal(remote.parseMessage(wire('input', {generation: 1,
    type: 'Runtime.evaluate'})), null);
  assert.equal(remote.parseMessage('x'.repeat(16385)), null);
});

test('host rejects a reply from a different pairing without applying SDP', async () => {
  const {session, peers} = fixture('host');
  const offer = await session.createOffer(stream());
  const pairing = remote.decodePairing(offer, 'offer');
  assert.notEqual(pairing.session, sessionId);
  await assert.rejects(() => session.acceptAnswer(
    remote.encodePairing(description('answer'), sessionId)), /不属于当前配对/);
  assert.equal(peers[0].remoteDescription, undefined);
  await session.acceptAnswer(remote.encodePairing(description('answer'), pairing.session));
  assert.equal(peers[0].remoteDescription.type, 'answer');
  session.close();
});

test('source changes reuse video and audio transceivers, including enabling audio later', async () => {
  const {session, peers} = fixture('host');
  const first = {id: 'first'};
  await session.createOffer(stream(first));
  const peer = peers[0];
  const senders = peer.transceivers.map(t => t.sender);
  assert.equal(peer.transceivers.length, 2);
  assert.equal(senders[0].track, first);
  assert.equal(senders[1].track, null);
  await session.replaceStream(null);
  const second = {id: 'second'};
  const audio = {id: 'audio'};
  await session.replaceStream(stream(second, audio));
  assert.equal(peer.transceivers.length, 2);
  assert.equal(peer.transceivers[0].sender, senders[0]);
  assert.equal(senders[0].track, second);
  assert.equal(senders[1].track, audio);
  session.close();
});

test('native input receives only granted, active and matching-generation events', async () => {
  const inputs = [];
  const {session, peers} = fixture('host', {onInput: input => inputs.push(clean(input))});
  await session.createOffer(stream());
  const channel = peers[0].channel;
  channel.open();
  session.setSource(source(2, true));
  channel.receive(wire('input', key(true, 1)));
  channel.receive(wire('source', {...source(2, true), revision: 99}));
  assert.equal(inputs.length, 0);
  channel.receive(wire('input', key(true, 2)));
  assert.equal(inputs.length, 1);
  session.setSource(source(2, false));
  channel.receive(wire('input', key(false, 2)));
  assert.equal(inputs.length, 1);
  channel.receive(wire('input', {type: 'release', generation: 2}));
  assert.equal(inputs.length, 2);
  session.setSource({...source(2, false), active: false});
  channel.receive(wire('input', {type: 'release', generation: 2}));
  assert.equal(inputs.length, 2);
  session.close();
});

test('pointer modifier combinations survive viewer and host validation', async () => {
  const inputs = [];
  const host = fixture('host', {onInput: input => inputs.push(clean(input))});
  const viewer = fixture('viewer');
  const offer = await host.session.createOffer(stream());
  await viewer.session.createAnswer(offer);
  const hostChannel = host.peers[0].channel;
  const viewerChannel = new FakeChannel();
  viewer.peers[0].ondatachannel({channel: viewerChannel});
  hostChannel.open();
  viewerChannel.open();
  host.session.setSource(source(2, true));
  viewerChannel.receive(JSON.stringify(hostChannel.sent.at(-1)));
  const shiftControl = remote.modifiersFor({shiftKey: true, ctrlKey: true});
  assert.equal(shiftControl, 10);
  const events = [
    {type: 'button', button: 0, down: true},
    {type: 'move'},
    {type: 'button', button: 0, down: false},
    {type: 'wheel', deltaX: 0, deltaY: 120},
  ];
  for (const event of events) {
    assert.equal(viewer.session.sendInput({...event, x: .25, y: .75,
      modifiers: shiftControl}), true);
    hostChannel.receive(JSON.stringify(viewerChannel.sent.at(-1)));
    assert.deepEqual(inputs.at(-1), {...event, x: .25, y: .75,
      modifiers: shiftControl, generation: 2});
    for (const modifiers of [-1, 16, .5, '8', null]) {
      assert.equal(viewer.session.sendInput({...event, x: .25, y: .75,
        modifiers}), false);
    }
  }
  assert.equal(inputs.length, events.length);
  host.session.close();
  viewer.session.close();
});

test('viewer rejects stale source grants and cannot grant itself control', async () => {
  const sources = [];
  const {session, peers} = fixture('viewer', {onSource: s => sources.push(clean(s))});
  await session.createAnswer(remote.encodePairing(description('offer'), sessionId));
  const channel = new FakeChannel();
  peers[0].ondatachannel({channel});
  channel.open();
  channel.receive(wire('source', {...source(3, true), revision: 2}));
  channel.receive(wire('source', {...source(2, true), revision: 90}));
  channel.receive(wire('source', {...source(3, true), revision: 1}));
  channel.receive(wire('source', {...source(3, false), revision: 3}));
  assert.equal(sources.length, 2);
  assert.equal(session.source.control, false);
  session.setSource({...source(3, true), revision: 4});
  assert.equal(session.source.control, false);
  assert.equal(session.sendInput(key()), false);
  session.close();
});

test('backpressure preserves a release rather than leaving a key held down', async () => {
  const {session, peers} = fixture('viewer');
  await session.createAnswer(remote.encodePairing(description('offer'), sessionId));
  const channel = new FakeChannel();
  peers[0].ondatachannel({channel});
  channel.open();
  channel.receive(wire('source', {...source(1, true), revision: 0}));
  assert.equal(session.sendInput(key()), true);
  channel.bufferedAmount = remote.MAX_BUFFERED_AMOUNT + 1;
  assert.equal(session.sendInput(key(false)), false);
  assert.equal(session.pendingRelease, true);
  channel.bufferedAmount = 0;
  channel.onbufferedamountlow();
  assert.equal(channel.sent.at(-1).input.type, 'release');
  assert.equal(session.pendingRelease, false);
  session.close();
});

test('closed peer callbacks cannot deliver input into a replacement session', async () => {
  const inputs = [];
  const {session, peers} = fixture('host', {onInput: e => inputs.push(e)});
  await session.createOffer(stream());
  session.setSource(source(1, true));
  const oldChannel = peers[0].channel;
  const oldMessage = oldChannel.onmessage;
  await session.createOffer(stream());
  session.setSource(source(1, true));
  oldMessage({data: wire('input', key())});
  assert.equal(inputs.length, 0);
  assert.equal(oldChannel.readyState, 'closed');
  session.close();
});

test('deferred release precedes new input before the buffer low event', async () => {
  const {session, peers} = fixture('viewer');
  await session.createAnswer(remote.encodePairing(description('offer'), sessionId));
  const channel = new FakeChannel();
  peers[0].ondatachannel({channel});
  channel.open();
  channel.receive(wire('source', {...source(1, true), revision: 0}));
  assert.equal(session.sendInput(key()), true);
  channel.bufferedAmount = remote.MAX_BUFFERED_AMOUNT + 1;
  assert.equal(session.sendInput(key(false)), false);
  assert.equal(session.sendInput(key()), false);
  assert.equal(channel.sent.length, 1);
  channel.bufferedAmount = remote.MAX_BUFFERED_AMOUNT * .75;
  assert.equal(session.sendInput(key()), true);
  assert.deepEqual(channel.sent.map(message => message.input.type),
    ['key', 'release', 'key']);
  channel.bufferedAmount = 0;
  channel.onbufferedamountlow();
  assert.equal(channel.sent.length, 3, 'A delayed release must not cancel the new key');
  session.close();
});

test('data channel errors revoke control before notifying either endpoint', async () => {
  for (const role of ['host', 'viewer']) {
    let interrupted = 0;
    const {session, peers} = fixture(role, {onInterrupted: reason => {
      assert.equal(reason, 'channel-error');
      assert.equal(session.source.control, false);
      ++interrupted;
    }});
    if (role === 'host') await session.createOffer(stream());
    else await session.createAnswer(remote.encodePairing(description('offer'), sessionId));
    const channel = role === 'host' ? peers[0].channel : new FakeChannel();
    if (role === 'viewer') peers[0].ondatachannel({channel});
    channel.open();
    if (role === 'host') session.setSource(source(1, true));
    else channel.receive(wire('source', {...source(1, true), revision: 0}));
    channel.onerror();
    assert.equal(interrupted, 1);
    assert.equal(session.source.control, false);
    if (role === 'viewer') assert.equal(session.sendInput(key()), false);
    session.close();
    channel.onerror();
    assert.equal(interrupted, 1, 'A stale channel must not interrupt a new session');
  }
});

test('ICE gathering completion and cancellation clean up pending listeners', async () => {
  const peer = new FakePeer({});
  peer.iceGatheringState = 'gathering';
  const pending = remote.waitForIce(peer, 1000);
  peer.iceGatheringState = 'complete';
  peer.dispatchEvent(new Event('icegatheringstatechange'));
  await pending;
  peer.iceGatheringState = 'gathering';
  const cancelled = remote.waitForIce(peer, 1000);
  peer.close();
  await assert.rejects(cancelled, /取消/);
  const timeoutPeer = new FakePeer({});
  timeoutPeer.iceGatheringState = 'gathering';
  await assert.rejects(remote.waitForIce(timeoutPeer, 5), /超时/);
});

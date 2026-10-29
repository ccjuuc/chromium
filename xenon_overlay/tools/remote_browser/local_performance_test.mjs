#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

// Real native tab capture, shipped UI and separate browser processes. GPU
// remains enabled. Measurements are local baselines, not cross-device results.
import assert from 'node:assert/strict';
import {randomUUID} from 'node:crypto';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {availableParallelism, tmpdir} from 'node:os';
import {basename, isAbsolute, join, relative, resolve} from 'node:path';
import {createViewerHandler} from './serve_viewer.mjs';
import {until, startBrowser, stopBrowser, newPage, ownedBrowsers,
  requestStop} from './local_pairing_test.mjs';

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const options = {seconds: 10, width: 1920, height: 1080, profile: 'default'};
for (let i = 2; i < process.argv.length; ++i) {
  const arg = process.argv[i];
  if (arg === '--help') {
    console.log('node local_performance_test.mjs --browser <exe> --report <new.json> ' +
      '[--seconds 10] [--width 1920] [--height 1080] [--headed] ' +
      '[--profile default|interactive] [--viewer-resources <directory>]');
    process.exit(0);
  }
  if (arg === '--headed') { options.headed = true; continue; }
  assert(['--browser', '--report', '--seconds', '--width', '--height', '--profile',
    '--viewer-resources'].includes(arg), arg);
  assert(process.argv[i + 1] && !process.argv[i + 1].startsWith('--'), `Missing ${arg}`);
  if (arg === '--profile') { options.profile = process.argv[++i]; continue; }
  options[arg.slice(2)] = ['--browser', '--report', '--viewer-resources'].includes(arg) ?
    resolve(process.argv[++i]) : Number(process.argv[++i]);
}
assert(options.browser && options.report, '--browser and --report are required');
assert(['default', 'interactive'].includes(options.profile));
assert(Number.isInteger(options.seconds) && options.seconds >= 5 && options.seconds <= 60);
for (const key of ['width', 'height']) assert(Number.isInteger(options[key]) &&
  options[key] >= 480 && options[key] <= 4096);

const title = `XenonPerf-${randomUUID()}`;
const fixture = `<!doctype html><meta charset="utf-8"><title>${title}</title>
<style>html,body{margin:0;background:#f4f6f9;color:#162232;font:20px sans-serif;overflow:hidden}
#marker{position:fixed;top:0;left:0;right:0;height:60px;background:rgb(210,30,30);z-index:2}
#rows{position:absolute;top:70px;left:30px;right:30px}
.row{height:48px;border-bottom:1px solid #c0cad5;display:flex;align-items:center;gap:60px}
.row:nth-child(even){background:#e5ebf1}button{position:fixed;left:40%;top:40%;width:20%;height:15%;z-index:3}
</style><div id="marker"></div><div id="rows">${Array.from({length: 80}, (_, i) =>
  `<div class="row"><b>${i + 1}. Remote browser performance</b><span>页面文字清晰度 0123456789</span><span>Status: ready</span></div>`).join('')}</div>
<button id="button">Click response / 点击反馈</button><script>
window.perfFixture={clicks:0,moves:0,scrolling:false,animationFrames:0};
document.querySelector('#button').onclick=e=>{
 if(!e.isTrusted)throw Error('Input must reach the native target');
 perfFixture.lastInputAt=Date.now();
 document.querySelector('#marker').style.backgroundColor=++perfFixture.clicks%2?'rgb(30,190,50)':'rgb(210,30,30)';
};
document.addEventListener('pointermove',e=>{if(e.isTrusted)++perfFixture.moves;});
function frame(t){++perfFixture.animationFrames;document.querySelector('#rows').style.transform=perfFixture.scrolling?
 'translateY(-'+(t*.18%1800)+'px)':'none';requestAnimationFrame(frame);}requestAnimationFrame(frame);
</script>`;
const viewerHandler = createViewerHandler(options['viewer-resources']);
const server = createServer((request, response) => {
  if (request.url === '/fixture') response.writeHead(200,
    {'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store'}).end(fixture);
  else void viewerHandler(request, response);
});
const temporary = await mkdtemp(join(tmpdir(), 'xenon-remote-perf-'));
const report = {started: new Date().toISOString(), options, gpuDisabled: false,
  scope: 'local native tab capture; GPU enabled; real input-to-decoded-pixel feedback',
  logicalProcessors: availableParallelism(), phases: [], passed: false};
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, requestStop);

async function sample(page) {
  return page.evaluate(`(async()=>{
    const peer=__localPairProbe.peers.at(-1), stats=await peer.getStats();
    const entries=[...stats.values()];
    const outbound=entries.find(s=>s.type==='outbound-rtp'&&s.kind==='video');
    const inbound=entries.find(s=>s.type==='inbound-rtp'&&s.kind==='video');
    const transport=entries.find(s=>s.type==='transport'&&s.selectedCandidatePairId);
    const pair=transport?stats.get(transport.selectedCandidatePairId):null;
    const fields=(s,keys)=>s?Object.fromEntries(keys.filter(k=>s[k]!==undefined).map(k=>[k,s[k]])):null;
    return {time:performance.now(),
      outbound:fields(outbound,['bytesSent','framesEncoded','framesPerSecond','frameWidth','frameHeight',
        'totalEncodeTime','totalPacketSendDelay','packetsSent','qualityLimitationReason',
        'encoderImplementation','powerEfficientEncoder','qpSum']),
      inbound:fields(inbound,['bytesReceived','framesDecoded','framesDropped','totalDecodeTime',
        'jitterBufferDelay','jitterBufferEmittedCount','totalProcessingDelay','decoderImplementation',
        'powerEfficientDecoder','freezeCount','totalFreezesDuration']),
      codec:stats.get((outbound||inbound)?.codecId)?.mimeType,
      pair:fields(pair,['currentRoundTripTime','availableOutgoingBitrate']),
      data:entries.filter(s=>s.type==='data-channel').map(s=>fields(s,['label','messagesSent','messagesReceived','bytesSent'])),
      buffered:__localPairProbe.channels.map(c=>c.bufferedAmount)};
  })()`);
}
async function cpu(browser) {
  return (await browser.cdp.send('SystemInfo.getProcessInfo')).processInfo;
}
function cpuPercent(before, after, seconds) {
  const previous = new Map(before.map(p => [p.id, p.cpuTime]));
  return after.reduce((sum, p) => sum + (previous.has(p.id) ?
    Math.max(0, p.cpuTime - previous.get(p.id)) : 0), 0) / seconds * 100;
}
function delta(a, b, key) { return (b?.[key] ?? 0) - (a?.[key] ?? 0); }
function rounded(value) { return Math.round(value * 100) / 100; }

try {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const baseUrl = `http://127.0.0.1:${server.address().port}`;
  const hostBrowser = await startBrowser('perf-host', options.browser, join(temporary, 'host'),
    [`--auto-select-tab-capture-source-by-title=${title}`,
      ...(!options.headed ? [`--screen-info={${Math.max(2560, options.width + 100)}x${Math.max(1600, options.height + 150)}}`] : [])],
    options.headed, {disableGpu: false});
  const viewerBrowser = await startBrowser('perf-viewer', options.browser, join(temporary, 'viewer'),
    [], options.headed, {disableGpu: false});
  report.browsers = {host: hostBrowser.version.product, viewer: viewerBrowser.version.product};
  const gpu = await hostBrowser.cdp.send('SystemInfo.getInfo');
  report.gpu = {devices: gpu.gpu.devices.map(d=>({vendor:d.vendorString,device:d.deviceString})),
    featureStatus: gpu.gpu.featureStatus};
  const target = await newPage(hostBrowser, `${baseUrl}/fixture`);
  const host = await newPage(hostBrowser, 'chrome://xenon-remote/', true);
  const viewer = await newPage(viewerBrowser, `${baseUrl}/`, true);
  const window = await hostBrowser.cdp.send('Browser.getWindowForTarget', {targetId: target.targetId});
  const geometry = await target.evaluate('({outerWidth,outerHeight,innerWidth,innerHeight})');
  await hostBrowser.cdp.send('Browser.setWindowBounds', {windowId:window.windowId,
    bounds:{width:options.width + Math.max(0,geometry.outerWidth-geometry.innerWidth),
      height:options.height + Math.max(0,geometry.outerHeight-geometry.innerHeight)}});
  await target.send('Page.bringToFront');
  await until('fixture window resized', async () => (await target.evaluate('innerWidth')) >= options.width);
  report.fixtureGeometry = await target.evaluate('({outerWidth,outerHeight,innerWidth,innerHeight})');
  await until('host ready', () => host.evaluate(`!document.querySelector('#choose-source').disabled`));
  await host.click('#choose-source');
  await until('capture ready', () => host.evaluate(`!document.querySelector('#create-offer').disabled`));
  await host.click('#create-offer');
  const offer = await until('offer', () => host.evaluate(`document.querySelector('#offer-output').value||null`));
  await viewer.fill('#offer-input', offer);
  await viewer.click('#create-answer');
  const answer = await until('answer', () => viewer.evaluate(`document.querySelector('#answer-output').value||null`));
  await host.fill('#answer-input', answer);
  await host.click('#accept-answer');
  await until('decoded video', async () => (await sample(viewer)).inbound?.framesDecoded > 0);
  await target.send('Page.bringToFront');
  await viewer.send('Page.bringToFront');
  await viewer.evaluate(`document.querySelector('#remote-video').scrollIntoView({block:'center'})`);
  report.visibility = await viewer.evaluate(`({state:document.visibilityState,
    video:document.querySelector('#remote-video').getBoundingClientRect().toJSON(),
    innerWidth,innerHeight})`);
  report.capture = await host.evaluate(`document.querySelector('#preview').srcObject.getVideoTracks()[0].getSettings()`);
  assert(report.capture.width >= options.width - 32 && report.capture.height >= options.height - 120,
    `Capture too small for benchmark: ${report.capture.width}x${report.capture.height}`);
  if (options.profile === 'interactive') {
    report.experimentalSender = await host.evaluate(`(async()=>{
      const sender=__localPairProbe.peers.at(-1).getSenders().find(s=>s.track?.kind==='video');
      sender.track.contentHint='text';
      const p=sender.getParameters();p.degradationPreference='maintain-resolution';
      for(const e of p.encodings){e.maxBitrate=8000000;e.maxFramerate=30;}
      await sender.setParameters(p);return sender.getParameters();
    })()`);
    await viewer.evaluate(`(()=>{for(const r of __localPairProbe.peers.at(-1).getReceivers()){
      if(r.track.kind==='video'){
        if('jitterBufferTarget' in r)r.jitterBufferTarget=0;
        else if('playoutDelayHint' in r)r.playoutDelayHint=0;
      }
    }})()`);
  }
  await delay(3000);
  for (const name of ['static-text', 'scrolling-text']) {
    console.log(`Measuring ${name} (${options.seconds}s)`);
    await target.evaluate(`perfFixture.scrolling=${name === 'scrolling-text'}`);
    await delay(2000);
    const [hostBefore, viewerBefore, hostCpuBefore, viewerCpuBefore, sourceBefore] = await Promise.all([
      sample(host), sample(viewer), cpu(hostBrowser), cpu(viewerBrowser),
      target.evaluate('({frames:perfFixture.animationFrames,visibility:document.visibilityState})')]);
    await delay(options.seconds * 1000);
    const [hostAfter, viewerAfter, hostCpuAfter, viewerCpuAfter, sourceAfter] = await Promise.all([
      sample(host), sample(viewer), cpu(hostBrowser), cpu(viewerBrowser),
      target.evaluate('({frames:perfFixture.animationFrames,visibility:document.visibilityState})')]);
    const seconds = (hostAfter.time - hostBefore.time) / 1000;
    const encoded = delta(hostBefore.outbound, hostAfter.outbound, 'framesEncoded');
    const decoded = delta(viewerBefore.inbound, viewerAfter.inbound, 'framesDecoded');
    const jitterFrames = delta(viewerBefore.inbound, viewerAfter.inbound, 'jitterBufferEmittedCount');
    const result = {name, durationSeconds: rounded(seconds),
      sourceAnimationFps: rounded((sourceAfter.frames - sourceBefore.frames) / seconds),
      sourceVisibility: [sourceBefore.visibility, sourceAfter.visibility],
      bitrateMbps: rounded(delta(hostBefore.outbound, hostAfter.outbound, 'bytesSent') * 8 / seconds / 1e6),
      encodedFps: rounded(encoded / seconds), decodedFps: rounded(decoded / seconds),
      encodeMsPerFrame: encoded ? rounded(delta(hostBefore.outbound, hostAfter.outbound, 'totalEncodeTime') * 1000 / encoded) : null,
      decodeMsPerFrame: decoded ? rounded(delta(viewerBefore.inbound, viewerAfter.inbound, 'totalDecodeTime') * 1000 / decoded) : null,
      jitterBufferMsPerFrame: jitterFrames ? rounded(delta(viewerBefore.inbound, viewerAfter.inbound, 'jitterBufferDelay') * 1000 / jitterFrames) : null,
      framesDropped: delta(viewerBefore.inbound, viewerAfter.inbound, 'framesDropped'),
      hostCpuPercentOfOneCore: rounded(cpuPercent(hostCpuBefore, hostCpuAfter, seconds)),
      viewerCpuPercentOfOneCore: rounded(cpuPercent(viewerCpuBefore, viewerCpuAfter, seconds)),
      hostStats: hostAfter, viewerStats: viewerAfter};
    report.phases.push(result);
    console.log(JSON.stringify(result));
  }
  await target.evaluate('perfFixture.scrolling=false');
  await host.click('#allow-control');
  await until('control enabled', async () => (await viewer.snapshot()).source?.control);
  await target.send('Page.bringToFront');
  await viewer.send('Page.bringToFront');
  await viewer.evaluate(`document.querySelector('#remote-video').scrollIntoView({block:'center'})`);
  await delay(2000);
  const latencies = [];
  const dispatchLatencies = [];
  for (let i = 0; i < 12; ++i) {
    const expectedGreen = i % 2 === 0;
    const point = await viewer.evaluate(`(()=>{
      const v=document.querySelector('#remote-video');v.scrollIntoView({block:'center'});
      const r=v.getBoundingClientRect(), s=Math.min(r.width/v.videoWidth,r.height/v.videoHeight);
      return {x:r.x+r.width/2,y:r.y+(r.height-v.videoHeight*s)/2+v.videoHeight*s*.475};
    })()`);
    await viewer.evaluate(`(()=>{
      const v=document.querySelector('#remote-video'), c=document.createElement('canvas');c.width=c.height=1;
      const ctx=c.getContext('2d',{willReadFrequently:true});
      globalThis.perfFeedback={result:null};
      v.addEventListener('pointerdown',()=>{
        const start=performance.now();
        perfFeedback.startAt=Date.now();
        const timer=setInterval(()=>{
          ctx.drawImage(v,v.videoWidth*.5,v.videoHeight*.025,1,1,0,0,1,1);
          const p=ctx.getImageData(0,0,1,1).data;
          if((${expectedGreen}?p[1]>p[0]+60:p[0]>p[1]+60)){
            clearInterval(timer);perfFeedback.result=performance.now()-start;
          }else if(performance.now()-start>5000){clearInterval(timer);perfFeedback.result=-1;}
        },10);
      },{once:true});
    })()`);
    await viewer.clickPoint(point);
    const elapsed = await until('click-to-video feedback', () => viewer.evaluate('perfFeedback.result'));
    assert(elapsed > 0, 'Click feedback timed out');
    latencies.push(elapsed);
    dispatchLatencies.push(await target.evaluate('perfFixture.lastInputAt') -
      await viewer.evaluate('perfFeedback.startAt'));
    await delay(200);
  }
  const sorted = [...latencies].sort((a,b)=>a-b);
  report.clickToDecodedPixelMs = {samples:latencies.map(rounded),
    median:rounded((sorted[5]+sorted[6])/2), p95:rounded(sorted[Math.ceil(sorted.length*.95)-1]),
    pollingResolutionMs:10};
  report.inputToTargetDomMs = dispatchLatencies;
  console.log(JSON.stringify({clickToDecodedPixelMs:report.clickToDecodedPixelMs}));
  const before = await sample(viewer);
  const injection = await viewer.evaluate(`(async()=>{
    const v=document.querySelector('#remote-video'), r=v.getBoundingClientRect();let emitted=0;
    const start=performance.now();
    const timer=setInterval(()=>{for(let burst=0;burst<4;++burst){v.dispatchEvent(new PointerEvent('pointermove',{
      pointerId:1,isPrimary:true,pointerType:'mouse',clientX:r.x+r.width*(.2+emitted%60/100),
      clientY:r.y+r.height*.5}));++emitted;}},2);
    await new Promise(resolve=>setTimeout(resolve,3000));clearInterval(timer);
    return {emitted,durationMs:performance.now()-start};
  })()`);
  await delay(500);
  const after = await sample(viewer);
  report.pointerBurst = {...injection,
    messagesSent:delta(before.data[0], after.data[0], 'messagesSent'),
    remainsControllable:(await viewer.snapshot()).source?.control};
  assert(report.pointerBurst.remainsControllable, 'Pointer burst revoked control');
  console.log(JSON.stringify({pointerBurst:report.pointerBurst}));
  await host.click('#stop');
  report.passed = true;
} catch (error) {
  report.error = error.stack || String(error);
  throw error;
} finally {
  report.finished = new Date().toISOString();
  await Promise.allSettled(ownedBrowsers.map(stopBrowser));
  await new Promise(resolve=>{server.close(resolve);server.closeAllConnections();});
  const owned = relative(resolve(tmpdir()), resolve(temporary));
  assert(owned && !isAbsolute(owned) && !owned.startsWith('..') &&
    basename(temporary).startsWith('xenon-remote-perf-'));
  await rm(temporary, {recursive:true,force:true,maxRetries:10,retryDelay:200});
  await writeFile(options.report, JSON.stringify(report,null,2)+'\n',{flag:'wx'});
  console.log(`Performance report: ${options.report}`);
}

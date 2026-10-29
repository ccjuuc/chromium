// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Shared real-WebUI assertions for the managed-certificate and GM smoke runs.
import assert from 'node:assert/strict';
import {setTimeout as delay} from 'node:timers/promises';

export async function checkCertificatePageActions(
    send, sessionId, localUrl, expectedGmEnabled = true) {
  const destination = new URL('/login?source=certificate-diagnostics', localUrl);
  assert(['127.0.0.1', 'localhost', '[::1]'].includes(destination.hostname),
      'Action checks must navigate only to an owned loopback test endpoint');
  assert.equal(destination.protocol, 'https:');
  const evaluate = async expression => {
    const result = await send('Runtime.evaluate', {
      expression, returnByValue: true, awaitPromise: true, userGesture: true,
    }, sessionId);
    assert(!result.exceptionDetails, JSON.stringify(result.exceptionDetails));
    return result.result?.value;
  };
  const setUrl = value => evaluate(`(() => {
    const input = document.getElementById('test-url');
    input.value = ${JSON.stringify(value)};
    input.dispatchEvent(new Event('input', {bubbles: true}));
  })()`);
  const initialTargets = new Set((await send('Target.getTargets')).targetInfos
      .map(target => target.targetId));

  await setUrl('https://test2.gmssl.cn');
  const mutualPreset = await evaluate(`({
    preset: document.getElementById('test-preset').value,
    hint: document.getElementById('preset-hint').textContent,
    probe: document.getElementById('test-button').textContent,
    open: document.getElementById('open-button').textContent,
  })`);
  const gmUi = await evaluate(`({
    presets: !!document.getElementById('gm-test-presets'),
    pin: !!document.getElementById('gm-pin-hint'),
  })`);
  assert.equal(gmUi.presets, expectedGmEnabled);
  assert.equal(gmUi.pin, expectedGmEnabled);
  if (expectedGmEnabled) {
    assert.equal(mutualPreset.preset, 'https://test2.gmssl.cn/');
    assert(mutualPreset.hint.includes('双向认证') &&
        mutualPreset.hint.includes('在浏览器中打开'));
  } else {
    assert.equal(mutualPreset.preset, '');
    assert(!mutualPreset.hint.includes('双向认证') &&
        !mutualPreset.hint.includes('USBKey'));
  }
  assert.equal(mutualPreset.probe, '测试连接');
  assert.equal(mutualPreset.open, '在浏览器中打开');

  for (const value of ['http://127.0.0.1/', 'javascript:void(0)',
    'https://user:secret@127.0.0.1/', 'https://127.0.0.1/#fragment',
    'https://127.0.0.1/' + 'x'.repeat(2048)]) {
    for (const action of ['test-button', 'open-button']) {
      await setUrl(value);
      const invalid = await evaluate(`(() => {
        document.getElementById(${JSON.stringify(action)}).click();
        return {
          rejected: document.getElementById('test-url').getAttribute('aria-invalid'),
          errorHidden: document.getElementById('url-error').hidden,
          probePending: document.getElementById('test-button').disabled,
        };
      })()`);
      assert.equal(invalid.rejected, 'true', `${action}: ${value}`);
      assert(!invalid.errorHidden && !invalid.probePending);
    }
  }
  assert.deepEqual(new Set((await send('Target.getTargets')).targetInfos
      .map(target => target.targetId)), initialTargets,
      'Invalid URLs must not open a tab');

  await setUrl(destination.href);
  const opening = await evaluate(`(() => {
    document.getElementById('open-button').click();
    return {
      preset: document.getElementById('test-preset').value,
      probe: document.getElementById('test-button').textContent,
      resultHidden: document.getElementById('test-result').hidden,
      invalid: document.getElementById('test-url').hasAttribute('aria-invalid'),
    };
  })()`);
  assert.equal(opening.preset, '');
  assert.equal(opening.probe, '测试连接');
  assert(opening.resultHidden && !opening.invalid,
      'Browser navigation must not present the previous HEAD result');
  let opened;
  try {
    for (let attempt = 0; attempt < 100; ++attempt) {
      opened = (await send('Target.getTargets')).targetInfos.find(target =>
        !initialTargets.has(target.targetId) && target.type === 'page' &&
        target.url === destination.href);
      if (opened) break;
      await delay(50);
    }
    assert(opened, 'Custom HTTPS path and query must open in a separate tab');
    const attached = await send('Target.attachToTarget', {
      targetId: opened.targetId, flatten: true,
    });
    const opener = await send('Runtime.evaluate', {
      expression: 'window.opener === null', returnByValue: true,
    }, attached.sessionId);
    assert.equal(opener.result?.value, true, 'New tab must have no opener');
  } finally {
    if (opened) await send('Target.closeTarget', {targetId: opened.targetId});
  }
}

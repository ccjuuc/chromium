// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Run with: node --test xenon_overlay/tools/test_crash_upload_test.mjs
// These tests use synthetic dumps and mocked processes, never a real browser.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {cleanupTrackedProcess, finishLog, matchesReport, openLogFile,
  ownedProcesses, readMinidumpIdentity} from './test_crash_upload.mjs';
import {processSnapshot, terminateProcesses} from './test_crash_upload.mjs';

function minidump({ptype = 'renderer', pid = 1234, dictionary = false,
                   conflicting = false} = {}) {
  const bytes = Buffer.alloc(2048);
  let cursor = 32;
  const allocate = size => { const rva = cursor; cursor += size; return rva; };
  const u32 = (offset, value) => bytes.writeUInt32LE(value, offset);
  const location = (offset, rva, size) => { u32(offset, size); u32(offset + 4, rva); };
  const string = value => {
    const encoded = Buffer.from(value);
    const rva = allocate(4 + encoded.length);
    u32(rva, encoded.length);
    encoded.copy(bytes, rva + 4);
    return rva;
  };
  bytes.write('MDMP');
  const directory = allocate(24);
  u32(8, 2);
  u32(12, directory);
  const misc = allocate(12);
  u32(directory, 15);
  location(directory + 4, misc, 12);
  u32(misc, 12);
  u32(misc + 4, 1);
  u32(misc + 8, pid);
  const crashpad = allocate(52);
  u32(directory + 12, 0x43500001);
  location(directory + 16, crashpad, 52);
  u32(crashpad, 1);
  const modules = allocate(16);
  location(crashpad + 44, modules, 16);
  u32(modules, 1);
  const module = allocate(28);
  location(modules + 8, module, 28);
  u32(module, 1);
  const name = string('ptype');
  const value = string(ptype);
  if (dictionary) {
    const entries = allocate(12);
    location(module + 12, entries, 12);
    u32(entries, 1);
    u32(entries + 4, name);
    u32(entries + 8, value);
  } else {
    const objects = allocate(16);
    location(module + 20, objects, 16);
    u32(objects, 1);
    u32(objects + 4, name);
    bytes.writeUInt16LE(1, objects + 8);
    u32(objects + 12, value);
  }
  if (conflicting) {
    const entries = allocate(12);
    location(crashpad + 36, entries, 12);
    u32(entries, 1);
    u32(entries + 4, name);
    u32(entries + 8, string('browser'));
  }
  return bytes.subarray(0, cursor);
}

test('reads PID and ptype from bounded Crashpad annotation structures', () => {
  for (const dictionary of [false, true]) {
    for (const ptype of ['renderer', 'browser', 'gpu-process']) {
      assert.deepEqual(readMinidumpIdentity(minidump({dictionary, ptype})), {pid: 1234, ptype});
    }
  }
});

test('rejects corrupt, absent, or conflicting dump identities', () => {
  assert.throws(() => readMinidumpIdentity(Buffer.from('MDMP')), /bounds/);
  const badRva = minidump();
  badRva.writeUInt32LE(0xffffffff, 40);
  assert.throws(() => readMinidumpIdentity(badRva), /bounds/);
  const badCount = minidump();
  badCount.writeUInt32LE(0xffffffff, 8);
  assert.throws(() => readMinidumpIdentity(badCount), /bounds/);
  const noCrashpad = minidump();
  noCrashpad.writeUInt32LE(1, 8);
  assert.throws(() => readMinidumpIdentity(noCrashpad), /identity/);
  assert.throws(() => readMinidumpIdentity(minidump({pid: 0})), /identity/);
  assert.throws(() => readMinidumpIdentity(minidump({conflicting: true})), /identity/);
});

test('both formats require post-baseline report, matching dump type and owned PID', () => {
  for (const format of ['binary', 'multipart']) {
    const report = {report_id: 'new', metadata: {format}, ptype: 'renderer',
      identity: readMinidumpIdentity(minidump())};
    const match = (value = report, baseline = new Set()) =>
      matchesReport(value, baseline, format, 'renderer', [1234]);
    assert.equal(match(), true);
    assert.equal(match(report, new Set(['new'])), false);
    assert.equal(match({...report, identity: {ptype: 'gpu-process', pid: 1234}}), false);
    assert.equal(match({...report, identity: {ptype: 'renderer', pid: 9999}}), false);
    assert.equal(match({...report, identity: null}), false);
    assert.equal(match({...report, metadata: {format: 'other'}}), false);
    assert.equal(match({...report, ptype: 'browser'}), format === 'binary');
  }
});

const processIdentity = (pid, parent_pid, created) => ({pid, parent_pid, created});
const first = '2026-10-25T00:00:00.0000000Z';
const second = '2026-10-25T00:00:01.0000000Z';
const third = '2026-10-25T00:00:02.0000000Z';

test('tracks new descendants and known orphans but excludes reused PIDs', () => {
  const root = processIdentity(10, 1, first);
  const child = processIdentity(11, 10, second);
  const item = {...root, descendants: [child]};
  const newGpu = processIdentity(12, 10, third);
  assert.deepEqual(ownedProcesses(item, [child, newGpu, root]).map(p => p.pid), [10, 11, 12]);
  assert.deepEqual(ownedProcesses(item, [child]).map(p => p.pid), [11]);
  const reusedRoot = processIdentity(10, 1, third);
  assert.deepEqual(ownedProcesses(item, [reusedRoot, newGpu]), []);
  assert.deepEqual(ownedProcesses(item, [processIdentity(11, 10, third)]), []);
});

test('cleanup retries failure and retains a descendant discovered during cleanup', async () => {
  const root = processIdentity(10, 1, first);
  const child = processIdentity(11, 10, second);
  const item = {...root, descendants: []};
  let current = [root, child];
  let attempts = 0;
  const outcome = await cleanupTrackedProcess(item, async () => current, async targets => {
    assert.equal(targets[0].pid, 10);
    attempts++;
    if (attempts === 1) throw new Error('access temporarily denied');
    current = [];
  });
  assert.equal(outcome.success, true);
  assert.equal(outcome.attempts, 2);
  assert.equal(outcome.errors.length, 1);
  assert.deepEqual(item.descendants, [child]);
});

test('unresolved cleanup fails and can be retried by the outer finally block', async () => {
  const root = processIdentity(10, 1, first);
  const item = {...root, descendants: []};
  let current = [root];
  const failed = await cleanupTrackedProcess(item, async () => current, async () => {
    throw new Error('access denied');
  });
  assert.equal(failed.success, false);
  assert.equal(failed.errors.length, 2);
  assert.deepEqual(failed.remaining, [root]);
  const retried = await cleanupTrackedProcess(item, async () => current, async () => { current = []; });
  assert.equal(retried.success, true);
});

test('cleanup verifies exit and does not terminate a reused or unidentified PID', async () => {
  const root = processIdentity(10, 1, first);
  const item = {...root, descendants: []};
  const stillAlive = await cleanupTrackedProcess(item, async () => [root], async () => {});
  assert.equal(stillAlive.success, false);
  let calls = 0;
  const reused = await cleanupTrackedProcess(item, async () => [processIdentity(10, 1, third)], async () => { calls++; });
  assert.equal(reused.success, true);
  const unknown = await cleanupTrackedProcess({pid: 10, descendants: []}, async () => [root], async () => { calls++; });
  assert.equal(unknown.success, false);
  assert.equal(calls, 0);
});

test('log creation and later disk errors reject without unhandled stream errors', async t => {
  const directory = await mkdtemp(path.join(tmpdir(), 'crash-runner-log-test-'));
  t.after(() => rm(directory, {recursive: true, force: true}));
  const file = path.join(directory, 'existing.log');
  await writeFile(file, 'keep');
  await assert.rejects(openLogFile(file), {code: 'EEXIST'});
  const log = await openLogFile(path.join(directory, 'disk-full.log'));
  log.stream.destroy(Object.assign(new Error('disk full'), {code: 'ENOSPC'}));
  await assert.rejects(finishLog(log), /disk full/);
  const good = await openLogFile(path.join(directory, 'complete.log'));
  good.stream.write('complete');
  await finishLog(good);
});

test('Windows handle cleanup checks creation time before stopping its own helper',
  {skip: process.platform !== 'win32', timeout: 45000}, async t => {
    const child = spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], {
      windowsHide: true, stdio: 'ignore',
    });
    await once(child, 'spawn');
    let identity;
    t.after(async () => {
      if (identity && child.exitCode === null) await terminateProcesses([identity]);
    });
    identity = (await processSnapshot()).find(entry => entry.pid === child.pid);
    assert.ok(identity);
    await terminateProcesses([{...identity, created: first}]);
    assert.ok((await processSnapshot()).some(entry => entry.pid === identity.pid && entry.created === identity.created));
    const exited = once(child, 'exit');
    await terminateProcesses([identity]);
    await exited;
    assert.equal((await processSnapshot()).some(entry => entry.pid === identity.pid && entry.created === identity.created), false);
  });

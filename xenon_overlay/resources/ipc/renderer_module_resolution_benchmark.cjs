// Same-process resolver benchmark using the path test's instrumented bridge.
// Usage: node renderer_module_resolution_benchmark.cjs before.js [results.json]
// Windows and POSIX path semantics both run in Node VM contexts on this host;
// these timings do not measure Chromium startup or real filesystem latency.
const {readFileSync, writeFileSync} = require('node:fs');
const {createRequire} = require('node:module');
const path = require('node:path');
const vm = require('node:vm');
const {performance} = require('node:perf_hooks');
const {pathToFileURL} = require('node:url');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

if (!process.argv[2]) {
  throw new Error('Usage: node renderer_module_resolution_benchmark.cjs before.js [results.json]');
}
const fixture = path.join(__dirname, 'renderer_path_url_unittest.cjs');
const fixtureRequire = createRequire(fixture);
const fixtureSource = readFileSync(fixture, 'utf8');
const testBoundary = fixtureSource.indexOf("test('both path implementations");
if (testBoundary < 0) throw new Error('Cannot locate the path test harness');
const harnessSource = fixtureSource.slice(0, testBoundary);
const iterations = 100000;
const warmupIterations = 10000;
const rounds = 5;
const sources = {
  before: readBootstrap(path.resolve(process.argv[2])),
  after: readBootstrap(path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js')),
};
const createRenderers = Object.fromEntries(Object.entries(sources).map(([label, source]) => {
  const harnessRequire = id => id === './bootstrap_test_support.cjs' ?
      {readBootstrap: () => source} : fixtureRequire(id);
  return [label, new Function('require', '__dirname',
      harnessSource + '\nreturn renderer;')(harnessRequire, __dirname)];
}));
const exercise = new vm.Script(
    `for (let i = 0; i < ${iterations}; ++i) require('./entry.js');`);
const warmup = new vm.Script(
    `for (let i = 0; i < ${warmupIterations}; ++i) require('./entry.js');`);

function createHarness(label, platform) {
  const root = platform === 'win32' ? 'D:\\fixture\\App' : '/srv/fixture/App';
  const nativePath = platform === 'win32' ? path.win32 : path.posix;
  return createRenderers[label](platform, {
    root,
    files: [[nativePath.join(root, 'entry.js'), 'module.exports = {ready: true};']],
    config: {rendererUrlMappings: [
      {sourcePathPrefix: root, targetBaseUrl: 'chrome://fixture/'},
    ]},
  });
}

function countOperations(label, platform) {
  const {context, calls} = createHarness(label, platform);
  let urlCalls = 0;
  context.URL = class extends URL {
    constructor(...args) { ++urlCalls; super(...args); }
  };
  context.require('./entry.js');
  const coldFilesystemIpc = calls.length;
  const coldUrlCalls = urlCalls;
  exercise.runInContext(context);
  return {coldFilesystemIpc, warmFilesystemIpc: calls.length - coldFilesystemIpc,
    warmUrlConstructorCalls: urlCalls - coldUrlCalls};
}

function countUrlParentResolutions(label, platform) {
  const nativePath = platform === 'win32' ? path.win32 : path.posix;
  const counts = {};
  for (const kind of ['chrome', 'file']) {
    const {context, calls, root} = createHarness(label, platform);
    const parent = kind === 'chrome' ? 'chrome://fixture/renderer.js' :
        pathToFileURL(nativePath.join(root, 'renderer.js'), {windows: platform === 'win32'}).href;
    let priorCalls = 0;
    counts[kind] = [];
    for (let iteration = 0; iteration < 4; ++iteration) {
      context.require('./entry.js', parent);
      counts[kind].push(calls.length - priorCalls);
      priorCalls = calls.length;
    }
  }
  return counts;
}

const median = values => [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
const result = {node: process.version, hostPlatform: process.platform,
  iterations, warmupIterations, rounds, scenarios: []};
for (const platform of ['win32', 'linux']) {
  const durations = {before: [], after: []};
  // Alternate ordering so one side does not always benefit from running last.
  for (let round = 0; round < rounds; ++round) {
    for (const label of round % 2 ? ['after', 'before'] : ['before', 'after']) {
      const {context} = createHarness(label, platform);
      context.require('./entry.js');
      warmup.runInContext(context);
      const start = performance.now();
      exercise.runInContext(context);
      durations[label].push(performance.now() - start);
    }
  }
  // Constructor instrumentation runs separately from timing measurements.
  const scenario = {runtimePlatform: platform};
  for (const label of ['before', 'after']) {
    scenario[label] = {
      durationMs: durations[label], medianMs: median(durations[label]),
      ...countOperations(label, platform),
      urlParentFilesystemIpc: countUrlParentResolutions(label, platform),
    };
  }
  result.scenarios.push(scenario);
}
const json = JSON.stringify(result, null, 2) + '\n';
if (process.argv[3]) writeFileSync(path.resolve(process.argv[3]), json);
process.stdout.write(json);

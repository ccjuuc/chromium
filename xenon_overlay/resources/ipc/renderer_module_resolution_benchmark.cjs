// Same-process resolver benchmark using the unit test's instrumented bridge.
// Usage: node renderer_module_resolution_benchmark.cjs path/to/before.js
const {readFileSync} = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {performance} = require('node:perf_hooks');

const harnessSource = readFileSync(
    path.join(__dirname, 'renderer_module_paths_unittest.cjs'), 'utf8')
    .split("test('disk aliases")[0];
const iterations = 100000;
for (const [label, source] of [
  ['before', path.resolve(process.argv[2])],
  ['after', path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js')],
]) {
  process.env.XENON_TEST_BOOTSTRAP = source;
  const createPathRenderer = new Function('require', '__dirname',
      harnessSource + '\nreturn createPathRenderer;')(require, __dirname);
  const {context, operations} = createPathRenderer(new Map([
    ['C:\\test-app\\entry.js', 'module.exports = {ready: true};'],
  ]));
  context.require('./entry.js');
  const coldFilesystemIpc = operations.length;
  vm.runInContext('for (let i = 0; i < 10000; ++i) require("./entry.js");', context);
  const start = performance.now();
  vm.runInContext(`for (let i = 0; i < ${iterations}; ++i) require('./entry.js');`, context);
  const durationMs = performance.now() - start;
  console.log(JSON.stringify({label, iterations, coldFilesystemIpc,
    warmFilesystemIpc: operations.length - coldFilesystemIpc, durationMs}));
}
delete process.env.XENON_TEST_BOOTSTRAP;

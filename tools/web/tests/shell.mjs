// Check startup diagnostics without loading the private game build.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
const html = await readFile(new URL('../../../web/shell.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
function shell({isolated = true, jspi = true, memory64 = true, gpu = true} = {}) {
  const elements = new Map();
  const document = {getElementById(id) {
    if (!elements.has(id)) elements.set(id, {style: {}, clientWidth: 1280, clientHeight: 720});
    return elements.get(id);
  }};
  const context = {document, location: {search: '', hash: ''}, URLSearchParams, Uint8Array,
    performance: {now: () => 1}, console: {log() {}}, crossOriginIsolated: isolated,
    devicePixelRatio: 1, navigator: {gpu: gpu ? {} : null},
    WebAssembly: {Suspending: jspi ? function () {} : undefined, validate: () => memory64}};
  context.window = {addEventListener() {}};
  vm.runInNewContext(script, context);
  return context;
}
assert.equal(shell().Module.noInitialRun, true);
assert.equal(shell().window.trialsHd.startupError, undefined);
assert.match(shell({isolated: false}).window.trialsHd.startupError, /Cross-origin isolation/);
assert.match(shell({jspi: false}).window.trialsHd.startupError, /Memory64 and JSPI/);
assert.match(shell({memory64: false}).window.trialsHd.startupError, /Memory64 and JSPI/);
assert.match(shell({gpu: false}).window.trialsHd.startupError, /WebGPU is unavailable/);
const fullscreen = shell();
fullscreen.document.fullscreenElement = {};
let exited = false;
fullscreen.document.exitFullscreen = async () => { exited = true; };
fullscreen.window.trialsHd.filesStatus('Game closed.');
assert.equal(exited, true);
assert.equal(fullscreen.document.getElementById('import-panel').hidden, false);
console.log('SHELL PASSED: main gate, capability diagnostics, fullscreen status/exit visibility');

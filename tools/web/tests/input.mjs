// Synthetic input-state regressions; no game code or browser required.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const source = await readFile(new URL('../../../web/input.js', import.meta.url), 'utf8');
const {WebInput, EVENT_TYPE} = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
globalThis.window = {devicePixelRatio: 2};
let pads = [];
Object.defineProperty(globalThis, 'navigator', {value: {getGamepads: () => pads}});
const canvas = {clientWidth: 640, clientHeight: 360, width: 640, height: 360,
  getBoundingClientRect: () => ({left: 10, top: 20, width: 640, height: 360})};
const input = new WebInput(canvas, new SharedArrayBuffer(4256));
input.updateDimensions();
assert.deepEqual(input.getCanvasCoords({clientX: 30, clientY: 50}), {x: 40, y: 60});
// Producer and consumer counters wrap independently after long sessions.
input.u32[input.OFF_EVENT_HEAD / 4] = 0;
input.u32[input.OFF_EVENT_TAIL / 4] = 0xffffff00;
assert.equal(input.pushEvent(EVENT_TYPE.KEY_UP, 4, 0, 0), false);
input.u32[input.OFF_EVENT_HEAD / 4] = 0xffffffff;
input.u32[input.OFF_EVENT_TAIL / 4] = 0xfffffffe;
assert.equal(input.pushEvent(EVENT_TYPE.KEY_UP, 4, 0, 0), true);
assert.equal(input.u32[input.OFF_EVENT_HEAD / 4], 0);
const buttons = Array.from({length: 17}, () => ({pressed: false, value: 0}));
buttons[0] = {pressed: true, value: 1};
buttons[7] = {pressed: true, value: 0.75};
pads = [{connected: true, buttons, axes: [0.5, -0.25, 0, 0]}];
input.pollGamepads();
const base = input.OFF_PADS / 4;
assert.equal(input.u32[base], 1);
assert.equal(input.u32[base + 1], 129);
assert.equal(input.f32[base + 2], 0.5);
assert.equal(input.f32[base + 7], 0.75);
assert.equal(input.u32[0] & 1, 0);
input.setSyntheticAxis('lt', 32767);
assert.equal(input.f32[base + 6], 1);
input.synthetic.active = false;
pads = [];
input.pollGamepads();
assert.deepEqual([...input.u32.slice(base, base + 8)], Array(8).fill(0));
input.u32[input.OFF_EVENT_TAIL / 4] = input.u32[input.OFF_EVENT_HEAD / 4];
let prevented = false;
input.handleWheel({clientX: 30, clientY: 50, deltaX: -12.4, deltaY: 120,
  preventDefault: () => { prevented = true; }});
const eventBase = input.OFF_EVENTS / 4;
assert.deepEqual([...input.i32.slice(eventBase, eventBase + 8)],
  [EVENT_TYPE.POINTER_MOVE, 0, 40, 60, EVENT_TYPE.WHEEL, 0, -12, 120]);
assert.equal(prevented, true);
globalThis.document = {fullscreenElement: null,
  exitFullscreen: async () => { document.fullscreenElement = null; }};
canvas.requestFullscreen = async () => { document.fullscreenElement = canvas; };
await input.toggleFullscreen();
assert.equal(document.fullscreenElement, canvas);
await input.toggleFullscreen();
assert.equal(document.fullscreenElement, null);
console.log('INPUT JS PASSED: ring wrap/full, resized pointer coordinates, gamepad/trigger merge, wheel position, fullscreen');

// CDP fixture: route headless pad commands through navigator.getGamepads(),
// not WebInput's synthetic-pad merge, to exercise the real controller path.
(() => {
  const pad = {
    id: 'Standard controller test fixture', index: 0, connected: true,
    mapping: 'standard', axes: [0, 0, 0, 0],
    buttons: Array.from({length: 17}, () => ({pressed: false, touched: false, value: 0})),
  };
  let buttonChanges = 0;
  Object.defineProperty(navigator, 'getGamepads', {value: () => [pad, null, null, null]});
  const button = (index, pressed) => {
    pad.buttons[index] = {pressed: !!pressed, touched: !!pressed, value: pressed ? 1 : 0};
    buttonChanges++;
  };
  const axis = (name, raw) => {
    const index = {lx: 0, ly: 1, rx: 2, ry: 3}[name];
    if (index !== undefined) pad.axes[index] = Math.max(-1, Math.min(1, raw / 32767));
    else {
      const index = name === 'lt' ? 6 : 7;
      const value = Math.max(0, Math.min(1, raw / 32767));
      pad.buttons[index] = {pressed: value > 0.5, touched: value > 0, value};
    }
  };
  // Keep the fixture hooks when the normal shell registers its synthetic hooks.
  Object.defineProperty(window, 'setSyntheticPadButton', {get: () => button, set: () => {}});
  Object.defineProperty(window, 'setSyntheticPadAxis', {get: () => axis, set: () => {}});
  window.controllerFixture = {pad, get buttonChanges() { return buttonChanges; }};
})();

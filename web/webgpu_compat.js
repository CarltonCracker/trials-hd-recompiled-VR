// web/webgpu_compat.js - Emscripten --pre-js (runs on the page and in every
// worker): browser compatibility shims for emdawnwebgpu.
//
// setBindGroup with dynamic offsets: emdawnwebgpu passes the whole wasm heap
// (HEAPU32) plus a start index and count. Firefox rejects typed-array views
// larger than 2 GiB ("Argument 3 can't be an ArrayBuffer or an ArrayBufferView
// larger than 2 GB") and the Memory64 heap is larger, so such calls pass just
// the offsets.
;(() => {
  const kMaxView = 0x7fffffff;
  for (const name of ['GPURenderPassEncoder', 'GPUComputePassEncoder', 'GPURenderBundleEncoder']) {
    const proto = globalThis[name] && globalThis[name].prototype;
    if (!proto || !proto.setBindGroup || proto.setBindGroup.trialsHdCompat) {
      continue;
    }
    const original = proto.setBindGroup;
    const wrapped = function(index, group, offsets, start, length) {
      if (arguments.length === 5 && offsets && offsets.byteLength > kMaxView) {
        return original.call(this, index, group, offsets.subarray(start, start + length), 0, length);
      }
      return original.apply(this, arguments);
    };
    wrapped.trialsHdCompat = true;
    proto.setBindGroup = wrapped;
  }
})();

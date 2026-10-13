# Direct3D runtime map (default.xex v0.0.1.2)

Research notes for the native renderer: where the game's statically linked Xbox 360
Direct3D runtime lives and what its functions do. Addresses and inferred roles only.
Regenerate the raw data with:

```sh
tools/xex_dump/xex_dump assets default.xex out/xex_dump   # built with the project
tools/d3d_survey.py out/xex_dump generated/default > out/d3d_survey.txt
```

## Method

Functions that talk to the GPU write PM4 type-3 packets into the command buffer.
`tools/d3d_survey.py` finds packet headers built from immediates (fixed-size packets via
`lis`/`ori`, variable-size ones via `oris 0xC000` + an or'ed opcode) and lists each
emitter's direct callers.

**Limit:** the hot draw path builds no headers from immediates — no indexed-draw
(`DRAW_INDX`) emitter shows up, and only one `SET_CONSTANT` writer. Like the Xbox 360
Direct3D runtime in general, it most likely copies precomputed packet templates from
data and flushes dirty state in bulk before each draw. Those functions need the
dynamic pass below.

## Findings so far

| Address | Emits / evidence | Inferred role |
| --- | --- | --- |
| 0x8212BE38 … 0x8216xxxx | first emitter at 0x8212BE38; nearly all emitters below 0x82150000 | Direct3D runtime code region |
| sub_8212BC68 | spin loop (`mr r31,r31` yield hints) on a flag at +0x2ABD of the device | wait for GPU (fence / ring space); hot in profiles |
| sub_82144440 | calls WAIT_REG_MEM emitters sub_821439E0, sub_8214D738; heads the hot game-thread stack | present / swap path |
| sub_82134DB0 | INDIRECT_BUFFER | command buffer kickoff |
| sub_82144BD0 | SET_CONSTANT (variable size) | register/constant block upload |
| sub_82142A08 | LOAD_ALU_CONSTANT | shader constant upload from memory |
| sub_8212C088, sub_82130010, sub_821389A0, sub_821428B8, sub_82144A80, sub_8214AD40, sub_8214D738, sub_8214F140 | IM_LOAD_IMMEDIATE (+ INVALIDATE_STATE) | shader microcode loads (immediate) |
| sub_8212FEB8, sub_82130010, sub_8213AA20, sub_82144D00 | DRAW_INDX_2 | auto-index draws (built-in quads/clears/resolves) |
| sub_821351B0, sub_82147760, sub_8214BFC8 | EVENT_WRITE_SHD | fence / timestamp writes |
| sub_8213EC20, sub_8213ECF0 | VIZ_QUERY | occlusion query begin/end |
| sub_824C9CD8, sub_824C9E98, sub_824CA058 | SET_STATE, IM_LOAD; no direct callers | reached through function pointers; separate renderer component |

## Dynamic pass

A diagnostic build (`-DTRIALSHD_CALL_TRACE=ON`, `tools/gen_trace_shims.py`) wraps every
function in 0x8212B000–0x82170000 with a counter and can sample its arguments
(`TRIALS_HD_CALL_TRACE_CSV`, `TRIALS_HD_CALL_TRACE_ARGS`, `TRIALS_HD_CALL_TRACE_ARGS_CSV`).
`--log_guest_fps=true` logs draws and resolves per frame from the command processor.

Reference workload: *Middle Name: Danger*, steady gameplay, 60 FPS, **~1480 draws and
20.5 resolves per frame**.

| Address | Calls | Arguments (r3 = device unless noted) | Role | Confidence |
| --- | --- | --- | --- | --- |
| 0x40001A80 | — | r3 of almost every call | Direct3D device object (heap) | high |
| sub_82131C18 | 0.65/draw | r4=4 (triangle list), r5=0, r6=0, r7=index count | DrawIndexedVertices(prim, base vertex, start index, count) | high |
| sub_82130D90, sub_82131270, sub_82131828 | 0.20/draw (D90) | — | other draw entry points (share the commit path) | medium |
| sub_82141E68 | 1.02/draw | r4=0, r5=0x4000 or 0x4400, r6=device+0x780 / +0x1780, r7=count | commit shader constants (VS @ reg 0x4000, PS @ 0x4400) from the device shadow | high |
| sub_82141D10, sub_82141AD0 | 0.77, 0.59/draw | register bases 0x4410, 0x2200/0x2180 | commit other register ranges (internal to draws) | medium |
| sub_821340F0 | 1.00/draw | r4=sampler (0,1), r5=texture object, r6=per-sampler dirty bit (0x80000000 >> sampler) | SetTexture(sampler, texture) | high |
| sub_8212E9D8 | 0.66/draw | r4=stream 0, r5=buffer object, r6=offset 0, r7=stride (24) | SetStreamSource | high |
| sub_8212EB80 | 0.65/draw | r4=object (vertex buffer + 0x30), r5=r6=0x1000xxxx | stream/declaration related | low |
| sub_8212DE68, sub_8212E010, sub_8212E1B0, sub_8212E5E0, sub_8212E630 | 0.78/draw each | r4=sampler 0..2, r5=value; called through a table (r8 = own address) | per-sampler state setters | medium |
| sub_8212C2D8 | 0.39/draw | 0, 0, 640, 336 | viewport or scissor rect | low |
| sub_82142A08 | 0.44/draw | r5=GPU physical address, r6=size | LOAD_ALU_CONSTANT (constant buffer from memory) | medium |
| sub_8214D738 | 1/resolve | r4=flags, r5=&source rect, r6=destination texture, r7=&dest point, r8=level | Resolve | high |
| sub_82132100, sub_82132410, sub_8214CF68 | 1/resolve | surface sizes (1280/640, 672), formats, physical addresses | resolve helpers | medium |
| sub_82144440 | 1/frame | — | present / swap | high |
| sub_8212BC68 | ~677k/frame | — | GPU-wait spin (polled); disappears with a native renderer | high |

### Design consequence: read state from the device's register shadow

The device object holds shadow copies of GPU registers (vertex shader constants at
device+0x780 = register 0x4000, pixel shader constants at device+0x1780 = 0x4400) and
commits them in ranges before each draw. Rather than hooking every one of the ~100
small render-state setters, the native renderer can hook a short list — draws, resolve,
present, render targets, shaders, textures/streams — and read the remaining state from
the shadow at draw time. The shadow uses the Xenos register layout, which ReXGlue's GPU
code already decodes (register file, `draw_util`).

Next: locate the rest of the shadow (render-state / fetch-constant ranges and their
dirty masks), confirm the draw variants, and build a frame recorder on these hooks
that dumps one frame's draw list (primitive, counts, streams, textures, constants).

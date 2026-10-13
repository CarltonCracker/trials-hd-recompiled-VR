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

## Next: dynamic pass

Count calls per function in the Direct3D region during real frames (per-function
counters in the generated code, or sampling) and match the counts against the draw,
resolve and swap counts the command processor sees per frame. Functions called once
per draw are the draw entry points; once per frame, the swap; and so on. That gives the
hook list for the native renderer: draws, render targets, resolves, textures, shaders,
vertex/index streams, render states, clears, present.

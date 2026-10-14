// Copies a rectangle of a single-sampled texture 1:1 into the render target
// (frontbuffer to guest output). The viewport covers the destination.

struct BlitConstants {
  source_offset: vec2i,
}
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var<uniform> constants: BlitConstants;

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> @builtin(position) vec4f {
  let uv = vec2f(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  return vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let coord = constants.source_offset + vec2i(position.xy);
  return vec4f(textureLoad(source, coord, 0).rgb, 1.0);
}

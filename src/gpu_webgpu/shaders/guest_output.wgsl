// Guest output painting: one bilinear pass from the guest output image into
// the letterboxed output rectangle, which the presenter sets as the viewport.

struct VsOut {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
}

// A triangle covering the viewport: uv (0,0) at the top left, (2,0), (0,2).
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VsOut {
  let uv = vec2f(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  var out: VsOut;
  out.position = vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  out.uv = uv;
  return out;
}

@group(0) @binding(0) var guest_output: texture_2d<f32>;
@group(0) @binding(1) var guest_output_sampler: sampler;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
  return vec4f(textureSample(guest_output, guest_output_sampler, in.uv).rgb, 1.0);
}

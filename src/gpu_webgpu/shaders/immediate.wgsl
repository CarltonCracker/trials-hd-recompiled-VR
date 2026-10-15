// Immediate drawer (ImGui and other UI): textured, vertex-colored primitives.
// Positions arrive already in clip space; the drawer converts them from the
// UI coordinate space on the CPU (UI vertex counts are small).

struct VsIn {
  @location(0) position: vec2f,
  @location(1) uv: vec2f,
  @location(2) color: vec4f,  // unorm8x4: ImGui's packed RGBA
}

struct VsOut {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
  @location(1) color: vec4f,
}

@vertex
fn vs_main(in: VsIn) -> VsOut {
  var out: VsOut;
  out.position = vec4f(in.position, 0.0, 1.0);
  out.uv = in.uv;
  out.color = in.color;
  return out;
}

@group(0) @binding(0) var ui_texture: texture_2d<f32>;
@group(0) @binding(1) var ui_sampler: sampler;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
  return in.color * textureSample(ui_texture, ui_sampler, in.uv);
}

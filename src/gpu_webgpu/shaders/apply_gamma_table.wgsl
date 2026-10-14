// rexgpu-webgpu - 256-entry table gamma application compute shader (S-20).
// Matches Xenos hardware table ramp behavior and SDK apply_gamma_table_cs.

struct GammaConstants {
  size: vec2u,
};

@group(0) @binding(0) var<uniform> constants: GammaConstants;
@group(0) @binding(1) var source_texture: texture_2d<f32>;
@group(0) @binding(2) var<storage, read> ramp_table: array<vec4f>;
@group(0) @binding(3) var dest_texture: texture_storage_2d<rgba8unorm, write>;

// Table gamma evaluation for a single color:
// Quantizes RGB channels into 256 entries and reads BGR components from the ramp.
fn apply_gamma_table(color: vec3f) -> vec3f {
  let indices = vec3u(clamp(color, vec3f(0.0), vec3f(1.0)) * 255.0 + 0.5);
  // Xenos gamma ramp table has B in .z, G in .y, R in .x
  let r = ramp_table[indices.x].z;
  let g = ramp_table[indices.y].y;
  let b = ramp_table[indices.z].x;
  return vec3f(r, g, b);
}

@compute @workgroup_size(16, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  if (gid.x >= constants.size.x || gid.y >= constants.size.y) {
    return;
  }
  let coord = vec2i(gid.xy);
  let src = textureLoad(source_texture, coord, 0);
  let gamma_rgb = apply_gamma_table(src.rgb);
  textureStore(dest_texture, coord, vec4f(gamma_rgb, src.a));
}

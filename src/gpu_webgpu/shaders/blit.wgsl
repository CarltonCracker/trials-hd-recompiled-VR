// Copies a rectangle of a single-sampled texture 1:1 into the render target
// (frontbuffer to guest output), applying the guest's gamma ramp. The
// viewport covers the destination.

struct BlitConstants {
  source_offset: vec2i,
  // Bits 0:1: gamma ramp - 0: none, 1: 256-entry table (8-bit frontbuffers),
  // 2: piecewise linear. Bits 8:9, 10:11: leading columns, rows that copy the
  // next one (the resolve's half-pixel offset fill at 2x/3x).
  gamma_mode: u32,
  padding: u32,
}
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var<uniform> constants: BlitConstants;
// 256 DC_LUT_30_COLOR entries (blue 0:9, green 10:19, red 20:29), or 128 x
// RGB DC_LUT_PWL_DATA entries (base 0:15, delta 16:31), as u32s.
@group(0) @binding(2) var<storage, read> gamma_ramp: array<u32>;

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> @builtin(position) vec4f {
  let uv = vec2f(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  return vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

fn apply_pwl(value_10b: u32, channel: u32) -> f32 {
  let pwl = gamma_ramp[(value_10b >> 3u) * 3u + channel];
  let value = f32(pwl & 0xFFFFu) + f32((value_10b & 7u) * (pwl >> 16u)) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let fill = vec2i(vec2u(constants.gamma_mode >> 8u, constants.gamma_mode >> 10u) & vec2u(3u));
  let coord = constants.source_offset + max(vec2i(position.xy), fill);
  let gamma_mode = constants.gamma_mode & 3u;
  var color = textureLoad(source, coord, 0).rgb;
  if (gamma_mode == 1u) {
    let index = vec3u(saturate(color) * 255.0 + 0.5);
    color = vec3f(f32((gamma_ramp[index.r] >> 20u) & 0x3FFu) / 1023.0,
                  f32((gamma_ramp[index.g] >> 10u) & 0x3FFu) / 1023.0,
                  f32(gamma_ramp[index.b] & 0x3FFu) / 1023.0);
  } else if (gamma_mode == 2u) {
    let value = vec3u(saturate(color) * 1023.0 + 0.5);
    color = vec3f(apply_pwl(value.r, 0u), apply_pwl(value.g, 1u), apply_pwl(value.b, 2u));
  }
  return vec4f(color, 1.0);
}

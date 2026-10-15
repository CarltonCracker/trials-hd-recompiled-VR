// rexgpu-webgpu - 128-segment PWL (piecewise-linear) gamma application compute shader (S-20).
// Matches Xenos hardware PWL ramp behavior, SDK apply_gamma_pwl_cs, and LinearToPWLGamma.

struct GammaConstants {
  size: vec2u,
};

struct PwlEntry {
  base: u32,
  slope: u32,
};

@group(0) @binding(0) var<uniform> constants: GammaConstants;
@group(0) @binding(1) var source_texture: texture_2d<f32>;
@group(0) @binding(2) var<storage, read> ramp_pwl: array<PwlEntry>;
@group(0) @binding(3) var dest_texture: texture_storage_2d<rgba8unorm, write>;

// Single-channel PWL evaluation from a 128-segment piecewise-linear ramp:
// Base and slope interpolation with 1/8 sub-steps and 1/(1023*64) normalization.
fn apply_gamma_pwl_channel(c: f32, channel_idx: u32) -> f32 {
  let c_clamped = clamp(c, 0.0, 1.0);
  let c_1023 = u32(c_clamped * 1023.0 + 0.5);
  let segment = c_1023 >> 3u;
  let fraction = c_1023 & 7u;
  let entry_idx = segment * 3u + channel_idx;
  let entry = ramp_pwl[entry_idx];
  let val = f32(entry.base) + f32(fraction * entry.slope) * 0.125;
  return clamp(val * 1.52737048e-5, 0.0, 1.0);
}

// Full RGB PWL gamma evaluation:
// Xenos PWL ramps store channels in BGR order (2=R, 1=G, 0=B).
fn apply_gamma_pwl(color: vec3f) -> vec3f {
  let r = apply_gamma_pwl_channel(color.r, 2u);
  let g = apply_gamma_pwl_channel(color.g, 1u);
  let b = apply_gamma_pwl_channel(color.b, 0u);
  return vec3f(r, g, b);
}

// Pure mathematical PWL gamma conversion functions matching SDK LinearToPWLGamma / PWLGammaToLinear:
fn xe_linear_to_pwl_gamma(linear: f32) -> f32 {
  let l = clamp(linear, 0.0, 1.0);
  var scale = 1023.0;
  var offset = 0.0;
  if (l >= 128.0 / 1023.0) {
    if (l >= 512.0 / 1023.0) {
      scale = 1023.0 / 8.0;
      offset = 128.0 / 255.0;
    } else {
      scale = 1023.0 / 4.0;
      offset = 64.0 / 255.0;
    }
  } else {
    if (l >= 64.0 / 1023.0) {
      scale = 1023.0 / 2.0;
      offset = 32.0 / 255.0;
    } else {
      scale = 1023.0;
      offset = 0.0;
    }
  }
  return trunc(l * scale) * (1.0 / 255.0) + offset;
}

fn xe_pwl_gamma_to_linear(gamma: f32) -> f32 {
  let g = clamp(gamma, 0.0, 1.0);
  var scale = 1.0 / 1024.0;
  var offset = 0.0;
  if (g >= 96.0 / 255.0) {
    if (g >= 192.0 / 255.0) {
      scale = 8.0 / 1024.0;
      offset = -1024.0;
    } else {
      scale = 4.0 / 1024.0;
      offset = -256.0;
    }
  } else {
    if (g >= 64.0 / 255.0) {
      scale = 2.0 / 1024.0;
      offset = -64.0;
    } else {
      scale = 1.0 / 1024.0;
      offset = 0.0;
    }
  }
  var linear = g * ((255.0 * 1024.0) * scale) + offset;
  linear += trunc(linear * scale);
  linear *= (1.0 / 1023.0);
  return clamp(linear, 0.0, 1.0);
}

@compute @workgroup_size(16, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  if (gid.x >= constants.size.x || gid.y >= constants.size.y) {
    return;
  }
  let coord = vec2i(gid.xy);
  let src = textureLoad(source_texture, coord, 0);
  let gamma_rgb = apply_gamma_pwl(src.rgb);
  textureStore(dest_texture, coord, vec4f(gamma_rgb, src.a));
}

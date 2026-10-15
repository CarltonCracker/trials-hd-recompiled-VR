// FXAA on the guest output (swap_post_effect = fxaa / fxaa_extreme), drawn as
// a fullscreen triangle. The source is the gamma-corrected frontbuffer with
// its luma in alpha (blit.wgsl, luma_alpha). Same algorithm and constants as
// the SDK's Vulkan backend (GetSwapFxaaComputeSource).

struct FxaaConstants {
  size_inv: vec2f,
  // 0: FXAA, 1: FXAA extreme quality.
  extreme: u32,
  padding: u32,
}
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
@group(0) @binding(2) var<uniform> constants: FxaaConstants;

const kLumaWeights = vec3f(0.299, 0.587, 0.114);
const kDirReduceMul = 0.125;
const kDirReduceMin = 1.0 / 128.0;

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> @builtin(position) vec4f {
  let uv = vec2f(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  return vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

fn sample_rgba(uv: vec2f) -> vec4f {
  return textureSampleLevel(source, source_sampler, uv, 0.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
  var edge_threshold = 0.166;
  var edge_threshold_min = 0.0833;
  var span_max = 8.0;
  if (constants.extreme != 0u) {
    edge_threshold = 0.063;
    edge_threshold_min = 0.0312;
    span_max = 12.0;
  }

  let texel = constants.size_inv;
  let uv = position.xy * texel;  // position.xy is the pixel center.
  let rgbm = sample_rgba(uv);

  let luma_m = rgbm.a;
  let luma_nw = sample_rgba(uv + vec2f(-texel.x, -texel.y)).a;
  let luma_ne = sample_rgba(uv + vec2f(texel.x, -texel.y)).a;
  let luma_sw = sample_rgba(uv + vec2f(-texel.x, texel.y)).a;
  let luma_se = sample_rgba(uv + vec2f(texel.x, texel.y)).a;

  let luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
  let luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
  let luma_range = luma_max - luma_min;

  var result = rgbm.rgb;
  if (luma_range >= max(edge_threshold_min, luma_max * edge_threshold)) {
    var dir = vec2f(-((luma_nw + luma_ne) - (luma_sw + luma_se)),
                    (luma_nw + luma_sw) - (luma_ne + luma_se));
    let dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * (0.25 * kDirReduceMul),
                         kDirReduceMin);
    let rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, vec2f(-span_max), vec2f(span_max)) * texel;

    let rgb_a = 0.5 * (sample_rgba(uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                       sample_rgba(uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    let rgb_b = rgb_a * 0.5 +
                0.25 * (sample_rgba(uv + dir * -0.5).rgb + sample_rgba(uv + dir * 0.5).rgb);
    let luma_b = dot(rgb_b, kLumaWeights);
    result = select(rgb_b, rgb_a, luma_b < luma_min || luma_b > luma_max);
  }
  return vec4f(result, 1.0);
}

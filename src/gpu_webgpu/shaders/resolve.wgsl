// Resolve (EDRAM copy) of a render-target rectangle into a texture holding the
// values a texture fetch of the destination format would read (section 8.8):
// sample selection, the copy's exponent bias, the destination number format,
// and the red/blue swap. The viewport covers the destination rectangle.

struct ResolveConstants {
  // Source pixel = source_offset + (fragment pixel - dest_offset).
  source_offset: vec2i,
  dest_offset: vec2i,
  // Multiplier: format range of the source (k_16_16 snorm hosts hold value/32)
  // times 2^copy_dest_exp_bias.
  scale: f32,
  // 0: clamp to 0...1 (unorm), 1: round to an integer (16-bit signed
  // integer), 2: round to an unsigned integer, 3: as is (float).
  mode: u32,
  // Bits 0:3: samples to average; bit 4: swap red and blue; bits 8:9, 10:11:
  // host columns, rows at the left and top of the resolve area that copy the
  // next one (resolution scaling with the half-pixel offset leaves them
  // undrawn; the SDK's resolve_resolution_scale_fill_half_pixel_offset).
  flags: u32,
  padding: u32,
}

@group(0) @binding(0) var color_source: texture_2d<f32>;
@group(0) @binding(1) var color_source_ms: texture_multisampled_2d<f32>;
@group(0) @binding(2) var depth_source: texture_depth_2d;
@group(0) @binding(3) var depth_source_ms: texture_depth_multisampled_2d;
@group(0) @binding(4) var<uniform> constants: ResolveConstants;

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> @builtin(position) vec4f {
  let uv = vec2f(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  return vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

fn source_pixel(position: vec4f) -> vec2i {
  let fill = vec2i(vec2u(constants.flags >> 8u, constants.flags >> 10u) & vec2u(3u));
  return constants.source_offset + max(vec2i(position.xy) - constants.dest_offset, fill);
}

fn finish(value: vec4f) -> vec4f {
  var v = value * constants.scale;
  switch (constants.mode) {
    case 0u: { v = saturate(v); }
    case 1u: {
      v = clamp(round(v), vec4f(-32768.0), vec4f(32767.0));
      // Store raw 16-bit values; each fetch supplies its own signedness.
      v = select(v, v + vec4f(65536.0), v < vec4f(0.0));
    }
    case 2u: { v = clamp(round(v), vec4f(0.0), vec4f(65535.0)); }
    default: {}
  }
  if ((constants.flags & 16u) != 0u) {
    v = v.bgra;
  }
  return v;
}

@fragment
fn fs_color(@builtin(position) position: vec4f) -> @location(0) vec4f {
  return finish(textureLoad(color_source, source_pixel(position), 0));
}

@fragment
fn fs_color_ms(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let pixel = source_pixel(position);
  var sum = vec4f(0.0);
  var count = 0.0;
  for (var i = 0u; i < 4u; i++) {
    if ((constants.flags & (1u << i)) != 0u) {
      sum += textureLoad(color_source_ms, pixel, i32(i));
      count += 1.0;
    }
  }
  return finish(sum / max(count, 1.0));
}

@fragment
fn fs_depth(@builtin(position) position: vec4f) -> @location(0) vec4f {
  return finish(vec4f(textureLoad(depth_source, source_pixel(position), 0)));
}

@fragment
fn fs_depth_ms(@builtin(position) position: vec4f) -> @location(0) vec4f {
  // Depth is never averaged; the first selected sample is used.
  let pixel = source_pixel(position);
  var sample = 0;
  for (var i = 3; i >= 0; i--) {
    if ((constants.flags & (1u << u32(i))) != 0u) {
      sample = i;
    }
  }
  return finish(vec4f(textureLoad(depth_source_ms, pixel, sample)));
}

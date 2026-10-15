// EDRAM ownership transfer (section 8.7). Render targets are host textures;
// when a target takes over EDRAM tiles last written by another target (a
// different format, sample count or pitch at overlapping tiles), each
// destination sample finds its EDRAM location, maps it back to the source
// target's pixel and sample, and converts the source's 32-bit EDRAM value to
// the destination format, in one fragment pass. Addressing follows
// XeEdramOffsetInts in the SDK's edram.xesli (32bpp only). Needs
// wgsl_prelude.wgsl.

struct TransferTarget {
  base_tiles: u32,
  pitch_tiles: u32,
  // 0: 1x, 1: 4x (2x is drawn at 4x).
  msaa_4x: u32,
  is_depth: u32,
  // Storage ColorRenderTargetFormat, or DepthRenderTargetFormat if is_depth.
  format: u32,
  // Host value = guest value * output_scale (k_16_16 on snorm).
  output_scale: f32,
  scale_x: u32,
  scale_y: u32,
  // Tiles to transfer, relative to base_tiles.
  tile_first: u32,
  tile_count: u32,
  // Host pixel rows covering the tiles, and the pitch in host pixels.
  row_first: u32,
  row_end: u32,
  width: u32,
  padding0: u32,
  padding1: u32,
  padding2: u32,
}

struct TransferConstants {
  dest: TransferTarget,
  source: TransferTarget,
}

@group(0) @binding(0) var<uniform> c: TransferConstants;
@group(0) @binding(1) var color_source: texture_2d<f32>;
@group(0) @binding(2) var color_source_ms: texture_multisampled_2d<f32>;
@group(0) @binding(3) var depth_source: texture_depth_2d;
@group(0) @binding(4) var depth_source_ms: texture_depth_multisampled_2d;
@group(0) @binding(5) var stencil_source: texture_2d<u32>;
@group(0) @binding(6) var stencil_source_ms: texture_multisampled_2d<u32>;

// Stencil bit set by the fs_*_stencil_bit entry points (WebGPU has no stencil
// export, so each bit is written by its own pass with that bit as the write
// mask).
override stencil_bit: u32 = 0u;
// The source and destination have the same format: the host value is copied
// as is (no conversion through the 32-bit EDRAM representation, no
// precision loss).
override passthrough: bool = false;

fn tile_size(t: TransferTarget) -> vec2u {
  return vec2u(80u * t.scale_x, 16u * t.scale_y);
}

// Depth tiles store their two 40-sample halves swapped (its own inverse).
fn depth_swap(t: TransferTarget, in_tile: vec2u) -> vec2u {
  if (t.is_depth == 0u) {
    return in_tile;
  }
  let half_width = tile_size(t).x >> 1u;
  return vec2u(select(in_tile.x + half_width, in_tile.x - half_width, in_tile.x >= half_width),
               in_tile.y);
}

// EDRAM location of a host sample of target t: x = absolute tile, y = sample
// index within the (scaled) tile; x = 0xFFFFFFFF outside the transferred tiles.
// At 4x, with the standard sample locations, sample index bit 0 is the
// horizontal sample and bit 1 the vertical one (as in the Vulkan backend).
fn edram_location(t: TransferTarget, pixel: vec2u, sample_index: u32) -> vec2u {
  var rt = pixel;
  if (t.msaa_4x != 0u) {
    rt = (pixel << vec2u(1u)) + vec2u(sample_index & 1u, (sample_index >> 1u) & 1u);
  }
  let size = tile_size(t);
  let tile_xy = rt / size;
  let tile = tile_xy.y * t.pitch_tiles + tile_xy.x;
  if (tile_xy.x >= t.pitch_tiles || tile < t.tile_first || tile - t.tile_first >= t.tile_count) {
    return vec2u(0xFFFFFFFFu, 0u);
  }
  let in_tile = depth_swap(t, rt - tile_xy * size);
  return vec2u((t.base_tiles + tile) % 2048u, in_tile.y * size.x + in_tile.x);
}

// The inverse: host pixel (xy) and sample (z) of target t at an EDRAM location.
fn edram_source(t: TransferTarget, location: vec2u) -> vec3u {
  let size = tile_size(t);
  let tile = (location.x + 2048u - t.base_tiles) % 2048u;
  let in_tile = depth_swap(t, vec2u(location.y % size.x, location.y / size.x));
  let rt = vec2u(tile % t.pitch_tiles, tile / t.pitch_tiles) * size + in_tile;
  if (t.msaa_4x != 0u) {
    return vec3u(rt >> vec2u(1u), ((rt.y & 1u) << 1u) | (rt.x & 1u));
  }
  return vec3u(rt, 0u);
}

fn pack(t: TransferTarget, v: vec4f, stencil: u32) -> u32 {
  if (t.is_depth != 0u) {
    if (t.format == 1u) {
      return (xe_float32_to_20e4(v.x, true) << 8u) | stencil;
    }
    return (u32(round(saturate(v.x) * 16777215.0)) << 8u) | stencil;
  }
  switch (t.format) {
    case 0u, 1u: {
      return pack4x8unorm(v);
    }
    case 2u, 10u: {
      let q = vec4u(round(saturate(v) * vec4f(1023.0, 1023.0, 1023.0, 3.0)));
      return q.x | (q.y << 10u) | (q.z << 20u) | (q.w << 30u);
    }
    case 3u, 12u: {
      return xe_float32_to_7e3(v.x) | (xe_float32_to_7e3(v.y) << 10u) |
             (xe_float32_to_7e3(v.z) << 20u) | (u32(round(saturate(v.w) * 3.0)) << 30u);
    }
    case 4u: {
      let q = vec2i(round(clamp(v.xy / t.output_scale * (32767.0 / 32.0), vec2f(-32768.0),
                                vec2f(32767.0))));
      return (u32(q.x) & 0xFFFFu) | (u32(q.y) << 16u);
    }
    case 6u: {
      return pack2x16float(v.xy);
    }
    default: {
      return bitcast<u32>(v.x);
    }
  }
}

fn unpack(t: TransferTarget, p: u32) -> vec4f {
  if (t.is_depth != 0u) {
    if (t.format == 1u) {
      return vec4f(xe_float20e4_to_32(p >> 8u));
    }
    return vec4f(f32(p >> 8u) * (1.0 / 16777215.0));
  }
  switch (t.format) {
    case 0u, 1u: {
      return unpack4x8unorm(p);
    }
    case 2u, 10u: {
      return xe_unpack_10_10_10_2_unorm(p);
    }
    case 3u, 12u: {
      return vec4f(xe_float7e3_to_32(p), xe_float7e3_to_32(p >> 10u), xe_float7e3_to_32(p >> 20u),
                   f32(p >> 30u) * (1.0 / 3.0));
    }
    case 4u: {
      let q = vec2f(f32(i32(p << 16u) >> 16u), f32(i32(p) >> 16u));
      return vec4f(q * (32.0 / 32767.0) * t.output_scale, 0.0, 1.0);
    }
    case 6u: {
      return vec4f(unpack2x16float(p), 0.0, 1.0);
    }
    default: {
      return vec4f(bitcast<f32>(p), 0.0, 0.0, 1.0);
    }
  }
}

// Source sample for a destination fragment (discarded outside the tiles).
fn source_of(position: vec4f, sample_index: u32) -> vec3u {
  let location = edram_location(c.dest, vec2u(position.xy), sample_index);
  if (location.x == 0xFFFFFFFFu) {
    discard;
  }
  return edram_source(c.source, location);
}

fn value_from_color(position: vec4f, sample_index: u32) -> vec4f {
  let s = source_of(position, sample_index);
  return textureLoad(color_source, s.xy, 0);
}

fn value_from_color_ms(position: vec4f, sample_index: u32) -> vec4f {
  let s = source_of(position, sample_index);
  return textureLoad(color_source_ms, s.xy, i32(s.z));
}

fn depth_from_depth(position: vec4f, sample_index: u32) -> f32 {
  let s = source_of(position, sample_index);
  return textureLoad(depth_source, s.xy, 0);
}

fn depth_from_depth_ms(position: vec4f, sample_index: u32) -> f32 {
  let s = source_of(position, sample_index);
  return textureLoad(depth_source_ms, s.xy, i32(s.z));
}

fn packed_from_color(position: vec4f, sample_index: u32) -> u32 {
  let s = source_of(position, sample_index);
  return pack(c.source, textureLoad(color_source, s.xy, 0), 0u);
}

fn packed_from_color_ms(position: vec4f, sample_index: u32) -> u32 {
  let s = source_of(position, sample_index);
  return pack(c.source, textureLoad(color_source_ms, s.xy, i32(s.z)), 0u);
}

fn packed_from_depth(position: vec4f, sample_index: u32) -> u32 {
  let s = source_of(position, sample_index);
  return pack(c.source, vec4f(textureLoad(depth_source, s.xy, 0)),
              textureLoad(stencil_source, s.xy, 0).x & 0xFFu);
}

fn packed_from_depth_ms(position: vec4f, sample_index: u32) -> u32 {
  let s = source_of(position, sample_index);
  return pack(c.source, vec4f(textureLoad(depth_source_ms, s.xy, i32(s.z))),
              textureLoad(stencil_source_ms, s.xy, i32(s.z)).x & 0xFFu);
}

// A full-screen triangle, scissored to the destination rows; per-sample
// shading.
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> @builtin(position) vec4f {
  let uv = vec2f(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  return vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

@fragment
fn fs_color_from_color(@builtin(position) position: vec4f,
                       @builtin(sample_index) sample_index: u32) -> @location(0) vec4f {
  if (passthrough) {
    return value_from_color(position, sample_index);
  }
  return unpack(c.dest, packed_from_color(position, sample_index));
}

@fragment
fn fs_color_from_color_ms(@builtin(position) position: vec4f,
                          @builtin(sample_index) sample_index: u32) -> @location(0) vec4f {
  if (passthrough) {
    return value_from_color_ms(position, sample_index);
  }
  return unpack(c.dest, packed_from_color_ms(position, sample_index));
}

@fragment
fn fs_color_from_depth(@builtin(position) position: vec4f,
                       @builtin(sample_index) sample_index: u32) -> @location(0) vec4f {
  return unpack(c.dest, packed_from_depth(position, sample_index));
}

@fragment
fn fs_color_from_depth_ms(@builtin(position) position: vec4f,
                          @builtin(sample_index) sample_index: u32) -> @location(0) vec4f {
  return unpack(c.dest, packed_from_depth_ms(position, sample_index));
}

@fragment
fn fs_depth_from_color(@builtin(position) position: vec4f,
                       @builtin(sample_index) sample_index: u32) -> @builtin(frag_depth) f32 {
  return unpack(c.dest, packed_from_color(position, sample_index)).x;
}

@fragment
fn fs_depth_from_color_ms(@builtin(position) position: vec4f,
                          @builtin(sample_index) sample_index: u32) -> @builtin(frag_depth) f32 {
  return unpack(c.dest, packed_from_color_ms(position, sample_index)).x;
}

@fragment
fn fs_depth_from_depth(@builtin(position) position: vec4f,
                       @builtin(sample_index) sample_index: u32) -> @builtin(frag_depth) f32 {
  if (passthrough) {
    return depth_from_depth(position, sample_index);
  }
  return unpack(c.dest, packed_from_depth(position, sample_index)).x;
}

@fragment
fn fs_depth_from_depth_ms(@builtin(position) position: vec4f,
                          @builtin(sample_index) sample_index: u32) -> @builtin(frag_depth) f32 {
  if (passthrough) {
    return depth_from_depth_ms(position, sample_index);
  }
  return unpack(c.dest, packed_from_depth_ms(position, sample_index)).x;
}

// Depth destination stencil, one bit per pass (stencil reference 0xFF, write
// mask 1 << stencil_bit, after the depth pass wrote 0).
@fragment
fn fs_stencil_bit_from_depth(@builtin(position) position: vec4f,
                             @builtin(sample_index) sample_index: u32) {
  if ((packed_from_depth(position, sample_index) & (1u << stencil_bit)) == 0u) {
    discard;
  }
}

@fragment
fn fs_stencil_bit_from_depth_ms(@builtin(position) position: vec4f,
                                @builtin(sample_index) sample_index: u32) {
  if ((packed_from_depth_ms(position, sample_index) & (1u << stencil_bit)) == 0u) {
    discard;
  }
}

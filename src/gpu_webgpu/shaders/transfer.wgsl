// EDRAM ownership transfer (section 8.7). Render targets are host textures;
// when a target takes over EDRAM tiles last written by another target (a
// different format, sample count or pitch at overlapping tiles), the old
// target's samples are stored to a guest-layout EDRAM buffer (cs_store_*) and
// loaded into the new one (fs_load_*). Addressing follows XeEdramOffsetInts in
// the SDK's edram.xesli (32bpp only). Needs wgsl_prelude.wgsl.

struct TransferConstants {
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

@group(0) @binding(0) var<uniform> c: TransferConstants;
@group(0) @binding(1) var<storage, read_write> edram: array<u32>;
@group(0) @binding(2) var<storage, read> edram_in: array<u32>;
@group(0) @binding(3) var color_source: texture_2d<f32>;
@group(0) @binding(4) var color_source_ms: texture_multisampled_2d<f32>;
@group(0) @binding(5) var depth_source: texture_depth_2d;
@group(0) @binding(6) var depth_source_ms: texture_depth_multisampled_2d;
@group(0) @binding(7) var stencil_source: texture_2d<u32>;
@group(0) @binding(8) var stencil_source_ms: texture_multisampled_2d<u32>;

// Stencil bit set by fs_load_stencil_bit (WebGPU has no stencil export, so
// each bit is written by its own pass with that bit as the write mask).
override stencil_bit: u32 = 0u;

// EDRAM dword index of a host sample, or 0xFFFFFFFF outside the transferred tiles.
fn edram_address(pixel: vec2u, sample_index: u32) -> u32 {
  var rt = pixel;
  if (c.msaa_4x != 0u) {
    rt = (pixel << vec2u(1u)) + vec2u((sample_index >> 1u) & 1u, sample_index & 1u);
  }
  let tile_size = vec2u(80u * c.scale_x, 16u * c.scale_y);
  let tile_xy = rt / tile_size;
  let tile = tile_xy.y * c.pitch_tiles + tile_xy.x;
  if (tile_xy.x >= c.pitch_tiles || tile < c.tile_first || tile - c.tile_first >= c.tile_count) {
    return 0xFFFFFFFFu;
  }
  var in_tile = rt - tile_xy * tile_size;
  if (c.is_depth != 0u) {
    // Depth tiles store their two 40-sample halves swapped.
    let half_width = tile_size.x >> 1u;
    in_tile.x = select(in_tile.x + half_width, in_tile.x - half_width, in_tile.x >= half_width);
  }
  return ((c.base_tiles + tile) % 2048u) * (tile_size.x * tile_size.y) +
         in_tile.y * tile_size.x + in_tile.x;
}

fn pack(v: vec4f, stencil: u32) -> u32 {
  if (c.is_depth != 0u) {
    if (c.format == 1u) {
      return (xe_float32_to_20e4(v.x, true) << 8u) | stencil;
    }
    return (u32(round(saturate(v.x) * 16777215.0)) << 8u) | stencil;
  }
  switch (c.format) {
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
      let q = vec2i(round(clamp(v.xy / c.output_scale * (32767.0 / 32.0), vec2f(-32768.0),
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

fn unpack(p: u32) -> vec4f {
  if (c.is_depth != 0u) {
    if (c.format == 1u) {
      return vec4f(xe_float20e4_to_32(p >> 8u));
    }
    return vec4f(f32(p >> 8u) * (1.0 / 16777215.0));
  }
  switch (c.format) {
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
      return vec4f(q * (32.0 / 32767.0) * c.output_scale, 0.0, 1.0);
    }
    case 6u: {
      return vec4f(unpack2x16float(p), 0.0, 1.0);
    }
    default: {
      return vec4f(bitcast<f32>(p), 0.0, 0.0, 1.0);
    }
  }
}

// Store: one invocation per host pixel of the rows covering the tiles.
fn store_pixel(id: vec3u) -> vec2u {
  return vec2u(id.x, c.row_first + id.y);
}

fn store_in_bounds(pixel: vec2u) -> bool {
  return pixel.x < c.width && pixel.y < c.row_end;
}

fn store(pixel: vec2u, sample_index: u32, value: vec4f, stencil: u32) {
  let address = edram_address(pixel, sample_index);
  if (address != 0xFFFFFFFFu) {
    edram[address] = pack(value, stencil & 0xFFu);
  }
}

@compute @workgroup_size(8, 8)
fn cs_store_color(@builtin(global_invocation_id) id: vec3u) {
  let pixel = store_pixel(id);
  if (store_in_bounds(pixel)) {
    store(pixel, 0u, textureLoad(color_source, pixel, 0), 0u);
  }
}

@compute @workgroup_size(8, 8)
fn cs_store_color_ms(@builtin(global_invocation_id) id: vec3u) {
  let pixel = store_pixel(id);
  if (store_in_bounds(pixel)) {
    for (var i = 0u; i < 4u; i++) {
      store(pixel, i, textureLoad(color_source_ms, pixel, i), 0u);
    }
  }
}

@compute @workgroup_size(8, 8)
fn cs_store_depth(@builtin(global_invocation_id) id: vec3u) {
  let pixel = store_pixel(id);
  if (store_in_bounds(pixel)) {
    store(pixel, 0u, vec4f(textureLoad(depth_source, pixel, 0)),
          textureLoad(stencil_source, pixel, 0).x);
  }
}

@compute @workgroup_size(8, 8)
fn cs_store_depth_ms(@builtin(global_invocation_id) id: vec3u) {
  let pixel = store_pixel(id);
  if (store_in_bounds(pixel)) {
    for (var i = 0u; i < 4u; i++) {
      store(pixel, i, vec4f(textureLoad(depth_source_ms, pixel, i)),
            textureLoad(stencil_source_ms, pixel, i).x);
    }
  }
}

// Load: a full-screen triangle, scissored to the rows; per-sample shading.
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> @builtin(position) vec4f {
  let uv = vec2f(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  return vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

fn load_packed(position: vec4f, sample_index: u32) -> u32 {
  let address = edram_address(vec2u(position.xy), sample_index);
  if (address == 0xFFFFFFFFu) {
    discard;
  }
  return edram_in[address];
}

fn load(position: vec4f, sample_index: u32) -> vec4f {
  return unpack(load_packed(position, sample_index));
}

@fragment
fn fs_load_color(@builtin(position) position: vec4f,
                 @builtin(sample_index) sample_index: u32) -> @location(0) vec4f {
  return load(position, sample_index);
}

@fragment
fn fs_load_depth(@builtin(position) position: vec4f,
                 @builtin(sample_index) sample_index: u32) -> @builtin(frag_depth) f32 {
  return load(position, sample_index).x;
}

// Depth target stencil, one bit per pass (stencil reference 0xFF, write mask
// 1 << stencil_bit, after fs_load_depth wrote 0).
@fragment
fn fs_load_stencil_bit(@builtin(position) position: vec4f,
                       @builtin(sample_index) sample_index: u32) {
  if ((load_packed(position, sample_index) & (1u << stencil_bit)) == 0u) {
    discard;
  }
}

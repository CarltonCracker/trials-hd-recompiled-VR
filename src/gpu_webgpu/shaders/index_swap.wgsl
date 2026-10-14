// rexgpu-webgpu - Index buffer endian swap and primitive topology conversion compute shaders (S-22).
// Supports 16-bit and 32-bit index buffers, endian swaps (8in16, 8in32, 16in32),
// and quad-list / triangle-fan to triangle-list conversion (matching SDK PrimitiveProcessor).

struct IndexParams {
  element_count: u32,     // Number of quads (quads_to_triangles), fan triangles (fan_to_triangles), or u32 words (swap)
  endian: u32,            // 0: none, 1: 8in16, 2: 8in32, 3: 16in32
  index_base: u32,        // Guest index base added to each index
  is_auto: u32,           // 1 if auto-indexed (0, 1, 2, 3...), 0 if read from src_indices
};

@group(0) @binding(0) var<uniform> params: IndexParams;
@group(0) @binding(1) var<storage, read> src_indices: array<u32>;
@group(0) @binding(2) var<storage, read_write> dst_indices: array<u32>;

fn swap_16(val: u32, endian: u32) -> u32 {
  if (endian == 1u) {
    return ((val & 0xFFu) << 8u) | ((val >> 8u) & 0xFFu);
  }
  return val & 0xFFFFu;
}

fn swap_32(val: u32, endian: u32) -> u32 {
  var r = val;
  if (endian == 1u || endian == 2u) {
    r = ((r & 0x00FF00FFu) << 8u) | ((r >> 8u) & 0x00FF00FFu);
  }
  if (endian == 2u || endian == 3u) {
    r = (r << 16u) | (r >> 16u);
  }
  return r;
}

fn get_src_16(idx: u32) -> u32 {
  if (params.is_auto != 0u) {
    return idx + params.index_base;
  }
  let word = src_indices[idx >> 1u];
  let raw = select(word & 0xFFFFu, word >> 16u, (idx & 1u) != 0u);
  return swap_16(raw, params.endian) + params.index_base;
}

fn get_src_32(idx: u32) -> u32 {
  if (params.is_auto != 0u) {
    return idx + params.index_base;
  }
  return swap_32(src_indices[idx], params.endian) + params.index_base;
}

// 1. Swap 16-bit indices in place or into a new buffer (2 indices per u32 word).
@compute @workgroup_size(64)
fn swap_indices_16(@builtin(global_invocation_id) gid: vec3u) {
  let word_idx = gid.x;
  if (word_idx < params.element_count) {
    let word = src_indices[word_idx];
    let lo = swap_16(word & 0xFFFFu, params.endian) + params.index_base;
    let hi = swap_16(word >> 16u, params.endian) + params.index_base;
    dst_indices[word_idx] = ((hi & 0xFFFFu) << 16u) | (lo & 0xFFFFu);
  }
}

// 2. Swap 32-bit indices.
@compute @workgroup_size(64)
fn swap_indices_32(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < params.element_count) {
    dst_indices[idx] = swap_32(src_indices[idx], params.endian) + params.index_base;
  }
}

// 3. Quad list -> triangle list conversion for 16-bit indices.
// Each quad produces 6 indices (v0, v1, v2; v0, v2, v3), packed into 3 u32 words.
@compute @workgroup_size(64)
fn quads_to_triangles_16(@builtin(global_invocation_id) gid: vec3u) {
  let quad_idx = gid.x;
  if (quad_idx < params.element_count) {
    let base_v = quad_idx * 4u;
    let v0 = get_src_16(base_v + 0u) & 0xFFFFu;
    let v1 = get_src_16(base_v + 1u) & 0xFFFFu;
    let v2 = get_src_16(base_v + 2u) & 0xFFFFu;
    let v3 = get_src_16(base_v + 3u) & 0xFFFFu;

    let out_word = quad_idx * 3u;
    dst_indices[out_word + 0u] = (v1 << 16u) | v0;
    dst_indices[out_word + 1u] = (v0 << 16u) | v2;
    dst_indices[out_word + 2u] = (v3 << 16u) | v2;
  }
}

// 4. Quad list -> triangle list conversion for 32-bit indices.
// Each quad produces 6 indices (v0, v1, v2; v0, v2, v3).
@compute @workgroup_size(64)
fn quads_to_triangles_32(@builtin(global_invocation_id) gid: vec3u) {
  let quad_idx = gid.x;
  if (quad_idx < params.element_count) {
    let base_v = quad_idx * 4u;
    let v0 = get_src_32(base_v + 0u);
    let v1 = get_src_32(base_v + 1u);
    let v2 = get_src_32(base_v + 2u);
    let v3 = get_src_32(base_v + 3u);

    let out_idx = quad_idx * 6u;
    dst_indices[out_idx + 0u] = v0;
    dst_indices[out_idx + 1u] = v1;
    dst_indices[out_idx + 2u] = v2;
    dst_indices[out_idx + 3u] = v0;
    dst_indices[out_idx + 4u] = v2;
    dst_indices[out_idx + 5u] = v3;
  }
}

// 5. Triangle fan -> triangle list conversion for 16-bit indices.
// Each triangle i produces (v_prev, v_curr, v_first). Pairs of triangles are packed into 3 u32 words.
@compute @workgroup_size(64)
fn fan_to_triangles_16(@builtin(global_invocation_id) gid: vec3u) {
  let pair_idx = gid.x;
  let tri0 = pair_idx * 2u;
  if (tri0 >= params.element_count) {
    return;
  }
  let v_first = get_src_16(0u) & 0xFFFFu;
  let v0_prev = get_src_16(tri0 + 1u) & 0xFFFFu;
  let v0_curr = get_src_16(tri0 + 2u) & 0xFFFFu;

  let out_word = pair_idx * 3u;
  dst_indices[out_word + 0u] = (v0_curr << 16u) | v0_prev;

  let tri1 = tri0 + 1u;
  if (tri1 < params.element_count) {
    let v1_prev = v0_curr;
    let v1_curr = get_src_16(tri1 + 2u) & 0xFFFFu;
    dst_indices[out_word + 1u] = (v1_prev << 16u) | v_first;
    dst_indices[out_word + 2u] = (v_first << 16u) | v1_curr;
  } else {
    dst_indices[out_word + 1u] = v_first;
  }
}

// 6. Triangle fan -> triangle list conversion for 32-bit indices.
// Each triangle i produces (v_prev, v_curr, v_first).
@compute @workgroup_size(64)
fn fan_to_triangles_32(@builtin(global_invocation_id) gid: vec3u) {
  let tri_idx = gid.x;
  if (tri_idx < params.element_count) {
    let v_first = get_src_32(0u);
    let v_prev = get_src_32(tri_idx + 1u);
    let v_curr = get_src_32(tri_idx + 2u);

    let out_idx = tri_idx * 3u;
    dst_indices[out_idx + 0u] = v_prev;
    dst_indices[out_idx + 1u] = v_curr;
    dst_indices[out_idx + 2u] = v_first;
  }
}

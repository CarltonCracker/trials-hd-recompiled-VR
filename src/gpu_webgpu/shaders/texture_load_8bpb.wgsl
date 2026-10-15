// rexgpu-webgpu - 8 bits per block texture load compute shader (S-19).
// Handles untiling (2D and 3D) and linear layout for 8-bit textures (k_8, k_8_A, etc.).
// Reads 8-byte blocks from source_data and packs into 16-byte words in dest_data.
// Matches SDK Vulkan texture_load_8bpb_cs byte-for-byte.

struct LoadConstants {
  is_tiled_3d_endian_scale: u32,
  guest_offset: u32,
  guest_pitch_aligned: u32,
  guest_z_stride_block_rows_aligned: u32,
  size_blocks: vec3<u32>,
  host_offset: u32,
  host_pitch: u32,
  height_texels: u32,
};

@group(0) @binding(0) var<uniform> constants: LoadConstants;
@group(0) @binding(1) var<storage, read> source_data: array<vec2<u32>>;
@group(0) @binding(2) var<storage, read_write> dest_data: array<vec4<u32>>;

fn xe_texture_tiled_offset_2d_8bpb(p: vec2<i32>, pitch_aligned: u32) -> i32 {
  let micro = (p.x & 7i) + ((p.y & 14i) << 2u);
  let offset = (((p.x >> 5u) + ((p.y >> 5u) * bitcast<i32>(pitch_aligned >> 5u))) << 7u) +
               ((micro & -16i) << 1u) + (micro & 15i) + ((p.y & 1i) << 4u);
  return (((offset & -512i) << 3u) +
          ((p.y & 16i) << 7u) +
          ((offset & 448i) << 2u) +
          (((((p.y & 8i) >> 2u) + (p.x >> 3u)) & 3i) << 6u) +
          (offset & 63i));
}

fn xe_texture_tiled_offset_3d_8bpb(p: vec3<i32>, pitch_aligned: u32, height_aligned: u32) -> i32 {
  let z_outer = p.z >> 2u;
  let micro = (((p.x & 7i) + ((p.y & 6i) << 2u)) << 6u) >> 6u;
  let offset_outer = ((p.y >> 3u) + z_outer) & 1i;
  let offset1 = offset_outer + ((((p.x >> 3u) + (offset_outer << 1u)) & 3i) << 1u);
  let macro_outer = ((p.y >> 4u) + (z_outer * bitcast<i32>(height_aligned >> 4u))) * bitcast<i32>(pitch_aligned >> 5u);
  let macro_val = (((p.x >> 5u) + macro_outer) << 6u) & 0x0FFFFFFFi;
  let offset2 = (((macro_val << 1u) + (micro & -16i)) << 1u) + (micro & 15i) +
                ((p.z & 3i) << 6u) + ((p.y & 1i) << 4u);
  return ((((((((offset1 & 1i) << 3u) + ((offset2 >> 6u) & 7i)) << 3u) + (offset1 & -2i)) << 2u) +
           (offset2 & -512i)) << 3u) + (offset2 & 63i);
}

@compute @workgroup_size(4, 32, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let block_coords = gid << vec3<u32>(4u, 0u, 0u);
  if (any(block_coords.xy >= constants.size_blocks.xy)) {
    return;
  }
  let p = bitcast<vec3<i32>>(block_coords);
  let is_tiled = (constants.is_tiled_3d_endian_scale & 1u) != 0u;
  let is_3d = (constants.is_tiled_3d_endian_scale & 2u) != 0u;

  var guest_byte_offset: i32;
  if (is_tiled) {
    if (is_3d) {
      guest_byte_offset = xe_texture_tiled_offset_3d_8bpb(p, constants.guest_pitch_aligned, constants.guest_z_stride_block_rows_aligned);
    } else {
      guest_byte_offset = xe_texture_tiled_offset_2d_8bpb(p.xy, constants.guest_pitch_aligned);
    }
  } else {
    guest_byte_offset = p.x + (((p.z * bitcast<i32>(constants.guest_z_stride_block_rows_aligned)) + p.y) * bitcast<i32>(constants.guest_pitch_aligned));
  }

  let src_idx = (bitcast<u32>(bitcast<i32>(constants.guest_offset) + guest_byte_offset)) >> 3u;
  let dst_idx = ((bitcast<u32>(p.x + (((p.z * bitcast<i32>(constants.size_blocks.y)) + p.y) * bitcast<i32>(constants.host_pitch)))) + constants.host_offset) >> 4u;

  let block_stride = select(8u, 64u, is_tiled) >> 3u;
  let b0 = source_data[src_idx];
  let b1 = source_data[src_idx + block_stride];
  dest_data[dst_idx] = vec4<u32>(b0.x, b0.y, b1.x, b1.y);
}

// rexgpu-webgpu - Float24 depth texture load compute shader (S-19).
// Handles untiling (2D and 3D), linear layout, endian swaps (8in16, 8in32, 16in32),
// and Float24 (20e4) [0..2) to IEEE float32 decoding for depth textures (k_24_8, k_24_8_FLOAT).
// Matches SDK Vulkan texture_load_depth_float_cs byte-for-byte.

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
@group(0) @binding(1) var<storage, read> source_data: array<vec4<u32>>;
@group(0) @binding(2) var<storage, read_write> dest_data: array<vec4<u32>>;

fn xe_texture_tiled_offset_2d_32bpb(p: vec2<i32>, pitch_aligned: u32) -> i32 {
  let micro = (((p.x & 7i) + ((p.y & 14i) << 2u)) << 2u);
  let offset = (((p.x >> 5u) + ((p.y >> 5u) * bitcast<i32>(pitch_aligned >> 5u))) << 9u) +
               ((micro & -16i) << 1u) + (micro & 15i) + ((p.y & 1i) << 4u);
  return (((offset & -512i) << 3u) +
          ((p.y & 16i) << 7u) +
          ((offset & 448i) << 2u) +
          (((((p.y & 8i) >> 2u) + (p.x >> 3u)) & 3i) << 6u) +
          (offset & 63i));
}

fn xe_texture_tiled_offset_3d_32bpb(p: vec3<i32>, pitch_aligned: u32, height_aligned: u32) -> i32 {
  let z_outer = p.z >> 2u;
  let micro = (((p.x & 7i) + ((p.y & 6i) << 2u)) << 8u) >> 6u;
  let offset_outer = ((p.y >> 3u) + z_outer) & 1i;
  let offset1 = offset_outer + ((((p.x >> 3u) + (offset_outer << 1u)) & 3i) << 1u);
  let macro_outer = ((p.y >> 4u) + (z_outer * bitcast<i32>(height_aligned >> 4u))) * bitcast<i32>(pitch_aligned >> 5u);
  let macro_val = (((p.x >> 5u) + macro_outer) << 8u) & 0x0FFFFFFFi;
  let offset2 = (((macro_val << 1u) + (micro & -16i)) << 1u) + (micro & 15i) +
                ((p.z & 3i) << 8u) + ((p.y & 1i) << 4u);
  return ((((((((offset1 & 1i) << 3u) + ((offset2 >> 6u) & 7i)) << 3u) + (offset1 & -2i)) << 2u) +
           (offset2 & -512i)) << 3u) + (offset2 & 63i);
}

fn xe_endian_swap(val: vec4<u32>, endian: u32) -> vec4<u32> {
  var r = val;
  if (endian == 1u || endian == 2u) {
    r = ((r & vec4<u32>(0x00FF00FFu)) << vec4<u32>(8u)) | ((r & vec4<u32>(0xFF00FF00u)) >> vec4<u32>(8u));
  }
  if (endian == 2u || endian == 3u) {
    r = (r << vec4<u32>(16u)) | (r >> vec4<u32>(16u));
  }
  return r;
}

fn xe_float20e4_to_32_vec4(v4: vec4<u32>) -> vec4<u32> {
  let v = v4 >> vec4<u32>(8u);
  let mant = v & vec4<u32>(0x000FFFFFu);
  let exp = v >> vec4<u32>(20u);
  let is_denorm = exp == vec4<u32>(0u);
  let flb = bitcast<vec4<u32>>(bitcast<vec4<i32>>(firstLeadingBit(mant)));
  let denorm_exp = flb + vec4<u32>(4294967277u);
  let norm_exp = select(exp, denorm_exp, is_denorm);
  let exp_res = (norm_exp + vec4<u32>(112u)) << vec4<u32>(23u);
  let denorm_mant = ((mant << (vec4<u32>(20u) - flb)) & vec4<u32>(0x000FFFFFu)) << vec4<u32>(3u);
  let norm_mant = mant << vec4<u32>(3u);
  let mant_res = select(norm_mant, denorm_mant, is_denorm);
  return select(exp_res | mant_res, vec4<u32>(0u), v == vec4<u32>(0u));
}

@compute @workgroup_size(4, 32, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let block_coords = gid << vec3<u32>(3u, 0u, 0u);
  if (any(block_coords.xy >= constants.size_blocks.xy)) {
    return;
  }
  let p = bitcast<vec3<i32>>(block_coords);
  let is_tiled = (constants.is_tiled_3d_endian_scale & 1u) != 0u;
  let is_3d = (constants.is_tiled_3d_endian_scale & 2u) != 0u;
  let endian = (constants.is_tiled_3d_endian_scale >> 2u) & 3u;

  var guest_byte_offset: i32;
  if (is_tiled) {
    if (is_3d) {
      guest_byte_offset = xe_texture_tiled_offset_3d_32bpb(p, constants.guest_pitch_aligned, constants.guest_z_stride_block_rows_aligned);
    } else {
      guest_byte_offset = xe_texture_tiled_offset_2d_32bpb(p.xy, constants.guest_pitch_aligned);
    }
  } else {
    guest_byte_offset = (p.x * 4i) + (((p.z * bitcast<i32>(constants.guest_z_stride_block_rows_aligned)) + p.y) * bitcast<i32>(constants.guest_pitch_aligned));
  }

  let src_idx = (bitcast<u32>(bitcast<i32>(constants.guest_offset) + guest_byte_offset)) >> 4u;
  let dst_idx = ((bitcast<u32>((p.x * 4i) + (((p.z * bitcast<i32>(constants.size_blocks.y)) + p.y) * bitcast<i32>(constants.host_pitch)))) + constants.host_offset) >> 4u;

  let block_stride = select(16u, 32u, is_tiled) >> 4u;
  dest_data[dst_idx + 0u] = xe_float20e4_to_32_vec4(xe_endian_swap(source_data[src_idx], endian));
  dest_data[dst_idx + 1u] = xe_float20e4_to_32_vec4(xe_endian_swap(source_data[src_idx + block_stride], endian));
}

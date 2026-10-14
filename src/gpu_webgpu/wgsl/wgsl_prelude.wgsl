// Helpers shared by every translated Xenos shader (wgsl_shader_translator).
// Semantics follow the SDK's ShaderInterpreter and SpirvShaderTranslator.

// Flush ALU inputs before applying arithmetic, preserving a zero's sign.
fn xe_flush_denormal(x: f32) -> f32 {
  let bits = bitcast<u32>(x);
  return bitcast<f32>(select(bits, bits & 0x80000000u, (bits & 0x7F800000u) == 0u));
}
fn xe_flush_denormal4(x: vec4f) -> vec4f {
  let bits = bitcast<vec4u>(x);
  return bitcast<vec4f>(select(bits, bits & vec4u(0x80000000u),
                             (bits & vec4u(0x7F800000u)) == vec4u(0u)));
}

// Direct3D 9 multiplication: 0 (or a denormal, flushed by the hardware) times
// anything, including infinity and NaN, is +0.
fn xe_mul(a: f32, b: f32) -> f32 {
  return select(a * b, 0.0, a == 0.0 || b == 0.0);
}
fn xe_mul4(a: vec4f, b: vec4f) -> vec4f {
  return select(a * b, vec4f(0.0), (a == vec4f(0.0)) | (b == vec4f(0.0)));
}
fn xe_dp(a: vec4f, b: vec4f, count: u32) -> f32 {
  let m = xe_mul4(a, b);
  var r = 0.0 + m.x;
  r += m.y;
  if (count >= 3u) { r += m.z; }
  if (count >= 4u) { r += m.w; }
  return r;
}

// Xenos saturation maps NaN and all negative values (including -0) to +0.
fn xe_saturate(x: f32) -> f32 {
  let bits = bitcast<u32>(x);
  if ((bits & 0x80000000u) != 0u || bits > 0x7F800000u) { return 0.0; }
  if (bits >= 0x3F800000u) { return 1.0; }
  return x;
}

// Select bits, not floats: drivers may fold float selects into min/max with
// different NaN behavior. Xenos returns b if either operand is NaN.
fn xe_ordered(a: f32, b: f32) -> bool {
  return max(bitcast<u32>(a) & 0x7FFFFFFFu, bitcast<u32>(b) & 0x7FFFFFFFu) <= 0x7F800000u;
}
fn xe_ge(a: f32, b: f32) -> bool { return xe_ordered(a, b) && a >= b; }
fn xe_lt(a: f32, b: f32) -> bool { return xe_ordered(a, b) && a < b; }
fn xe_max(a: f32, b: f32) -> f32 {
  return bitcast<f32>(select(bitcast<u32>(b), bitcast<u32>(a), xe_ge(a, b)));
}
fn xe_min(a: f32, b: f32) -> f32 {
  return bitcast<f32>(select(bitcast<u32>(b), bitcast<u32>(a), xe_lt(a, b)));
}
fn xe_max4v(a: vec4f, b: vec4f) -> vec4f {
  return vec4f(xe_max(a.x, b.x), xe_max(a.y, b.y), xe_max(a.z, b.z), xe_max(a.w, b.w));
}
fn xe_min4v(a: vec4f, b: vec4f) -> vec4f {
  return vec4f(xe_min(a.x, b.x), xe_min(a.y, b.y), xe_min(a.z, b.z), xe_min(a.w, b.w));
}

fn xe_max4(a: vec4f) -> f32 {
  if (xe_ge(a.x, a.y) && xe_ge(a.x, a.z) && xe_ge(a.x, a.w)) { return a.x; }
  if (xe_ge(a.y, a.z) && xe_ge(a.y, a.w)) { return a.y; }
  if (xe_ge(a.z, a.w)) { return a.z; }
  return a.w;
}

const xe_flt_max: f32 = 3.40282346638528859812e+38;

fn xe_is_inf(x: f32) -> bool {
  return (bitcast<u32>(x) & 0x7FFFFFFFu) == 0x7F800000u;
}
fn xe_is_finite(x: f32) -> bool {
  return (bitcast<u32>(x) & 0x7F800000u) != 0x7F800000u;
}

// WGSL does not define division/log/sqrt at zero or outside their domain.
// Keep bitcasts input-dependent: WGSL rejects constant NaN/infinity expressions.
fn xe_rcp(x: f32) -> f32 {
  let bits = bitcast<u32>(x);
  if ((bits & 0x7FFFFFFFu) == 0u) { return bitcast<f32>((bits & 0x80000000u) | 0x7F800000u); }
  return 1.0 / x;
}
fn xe_rsq(x: f32) -> f32 {
  let bits = bitcast<u32>(x);
  if ((bits & 0x7FFFFFFFu) == 0u) { return bitcast<f32>((bits & 0x80000000u) | 0x7F800000u); }
  if ((bits & 0x80000000u) != 0u) { return bitcast<f32>((bits & 0u) | 0x7FC00000u); }
  return 1.0 / sqrt(x);
}
fn xe_log2(x: f32) -> f32 {
  let bits = bitcast<u32>(x);
  if ((bits & 0x7FFFFFFFu) == 0u) { return bitcast<f32>((bits & 0u) | 0xFF800000u); }
  if ((bits & 0x80000000u) != 0u) { return bitcast<f32>((bits & 0u) | 0x7FC00000u); }
  return log2(x);
}

// Clamping variants of the reciprocal and logarithm.
fn xe_clamp_inf_to_max(x: f32) -> f32 {
  return select(x, select(xe_flt_max, -xe_flt_max, x < 0.0), xe_is_inf(x));
}
fn xe_clamp_inf_to_zero(x: f32) -> f32 {
  return select(x, bitcast<f32>(bitcast<u32>(x) & 0x80000000u), xe_is_inf(x));
}

// cube: operand components zxyw of the coordinate (see the interpreter):
// returns T, S, 2 * major axis, face ID.
fn xe_cube(v: vec4f) -> vec4f {
  let x = v.z;
  let y = v.w;
  let z = v.x;
  let ax = abs(x);
  let ay = abs(y);
  let az = abs(z);
  var r: vec4f;
  if (az >= ax && az >= ay) {
    r = vec4f(-y, select(x, -x, z < 0.0), z, select(4.0, 5.0, z < 0.0));
  } else if (ay >= ax) {
    r = vec4f(select(z, -z, y < 0.0), x, y, select(2.0, 3.0, y < 0.0));
  } else {
    r = vec4f(-y, select(-z, z, x < 0.0), x, select(0.0, 1.0, x < 0.0));
  }
  r.z *= 2.0;
  return r;
}

// a0 from mova / maxa: floor(clamp(x, -256, 255) + 0.5), or without the 0.5.
fn xe_address(x: f32, round: bool) -> i32 {
  let value = select(-256.0, x, xe_ordered(x, x));
  return i32(floor(clamp(value, -256.0, 255.0) + select(0.0, 0.5, round)));
}

// Xenos endianness modes (xenos::Endian): 0 none, 1 8in16, 2 8in32, 3 16in32.
fn xe_endian_swap(v: u32, endian: u32) -> u32 {
  var r = v;
  if (endian == 1u || endian == 2u) {
    r = ((r & 0x00FF00FFu) << 8u) | ((r >> 8u) & 0x00FF00FFu);
  }
  if (endian == 2u || endian == 3u) {
    r = (r << 16u) | (r >> 16u);
  }
  return r;
}
fn xe_endian_swap4(v: vec4u, endian: u32) -> vec4u {
  return vec4u(xe_endian_swap(v.x, endian),
               xe_endian_swap(v.y, endian),
               xe_endian_swap(v.z, endian),
               xe_endian_swap(v.w, endian));
}

// Float24 (20e4) [0, 2) depth conversion to IEEE float32.
fn xe_float20e4_to_32(f24: u32) -> f32 {
  let val = f24 & 0xFFFFFFu;
  if (val == 0u) {
    return 0.0;
  }
  var mantissa = val & 0xFFFFFu;
  var exponent = val >> 20u;
  if (exponent == 0u) {
    let lz = countLeadingZeros(mantissa) - 11u;
    exponent = 1u - lz;
    mantissa = (mantissa << lz) & 0xFFFFFu;
  }
  return bitcast<f32>(((exponent + 112u) << 23u) | (mantissa << 3u));
}

// Float32 to Xenos 20e4 depth conversion with optional round to nearest even.
fn xe_float32_to_20e4(f: f32, round_to_nearest_even: bool) -> u32 {
  if (!(f > 0.0)) {
    return 0u;
  }
  let bits = bitcast<u32>(f);
  if (bits >= 0x3FFFFFF8u) {
    return 0xFFFFFFu;
  }
  var f32u32 = bits;
  if (f32u32 < 0x38800000u) {
    let shift = min(113u - (f32u32 >> 23u), 24u);
    f32u32 = (0x800000u | (f32u32 & 0x7FFFFFu)) >> shift;
  } else {
    f32u32 = f32u32 + 0xC8000000u;
  }
  if (round_to_nearest_even) {
    f32u32 += 3u + ((f32u32 >> 3u) & 1u);
  }
  return (f32u32 >> 3u) & 0xFFFFFFu;
}

// Xenos 7e3 [0, 32) color component in bits 0:9 to IEEE float32.
fn xe_float7e3_to_32(f10: u32) -> f32 {
  let val = f10 & 0x3FFu;
  if (val == 0u) {
    return 0.0;
  }
  var mantissa = val & 0x7Fu;
  var exponent = val >> 7u;
  if (exponent == 0u) {
    let lz = countLeadingZeros(mantissa) - 24u;
    exponent = 1u - lz;
    mantissa = (mantissa << lz) & 0x7Fu;
  }
  return bitcast<f32>(((exponent + 124u) << 23u) | (mantissa << 16u));
}

// IEEE float32 to Xenos 7e3 color conversion (clamped to [0, 31.875]).
fn xe_float32_to_7e3(f: f32) -> u32 {
  if (!(f > 0.0)) {
    return 0u;
  }
  let clamped_f = clamp(f, 0.0, 31.875);
  let f32_u = bitcast<u32>(clamped_f);
  let denormal_shift = min(125u - (f32_u >> 23u), 24u);
  let denormal = ((f32_u & 0x7FFFFFu) | 0x800000u) >> denormal_shift;
  let normal = f32_u - (124u << 23u);
  let biased = select(normal, denormal, f32_u < 0x3E800000u);
  let rounded = biased + 0x7FFFu + ((biased >> 16u) & 1u);
  return (rounded >> 16u) & 0x3FFu;
}

// Unsigned 10:10:10:2 unorm unpack to vec4f in [0, 1].
fn xe_unpack_10_10_10_2_unorm(packed: u32) -> vec4f {
  let x = f32(packed & 0x3FFu) * (1.0 / 1023.0);
  let y = f32((packed >> 10u) & 0x3FFu) * (1.0 / 1023.0);
  let z = f32((packed >> 20u) & 0x3FFu) * (1.0 / 1023.0);
  let w = f32(packed >> 30u) * (1.0 / 3.0);
  return vec4f(x, y, z, w);
}

// Signed 10:10:10:2 snorm unpack to vec4f in [-1, 1].
fn xe_unpack_10_10_10_2_snorm(packed: u32) -> vec4f {
  let x = max(f32(extractBits(bitcast<i32>(packed), 0u, 10u)) * (1.0 / 511.0), -1.0);
  let y = max(f32(extractBits(bitcast<i32>(packed), 10u, 10u)) * (1.0 / 511.0), -1.0);
  let z = max(f32(extractBits(bitcast<i32>(packed), 20u, 10u)) * (1.0 / 511.0), -1.0);
  let w = max(f32(extractBits(bitcast<i32>(packed), 30u, 2u)) * (1.0 / 1.0), -1.0);
  return vec4f(x, y, z, w);
}

// Signed 10:10:10:2 snorm with no_zero repeating fraction mode.
fn xe_unpack_10_10_10_2_snorm_no_zero(packed: u32) -> vec4f {
  let x = (f32(extractBits(bitcast<i32>(packed), 0u, 10u)) + 0.5) * (1.0 / 511.5);
  let y = (f32(extractBits(bitcast<i32>(packed), 10u, 10u)) + 0.5) * (1.0 / 511.5);
  let z = (f32(extractBits(bitcast<i32>(packed), 20u, 10u)) + 0.5) * (1.0 / 511.5);
  let w = (f32(extractBits(bitcast<i32>(packed), 30u, 2u)) + 0.5) * (1.0 / 1.5);
  return vec4f(x, y, z, w);
}

// 16-bit packed format unpacks.
fn xe_unpack_16_16_float(packed: u32) -> vec2f {
  return unpack2x16float(packed);
}

fn xe_unpack_16_16_snorm(packed: u32) -> vec2f {
  return unpack2x16snorm(packed);
}

fn xe_unpack_16_16_unorm(packed: u32) -> vec2f {
  return unpack2x16unorm(packed);
}

// Xenos k_16_16 RT ±32 range scaling.
// Host snorm16 holds [-1, 1], Xenos k_16_16 holds [-32, 32].
fn xe_scale_k_16_16_to_host(val: vec2f) -> vec2f {
  return val * 0.03125;
}

fn xe_scale_k_16_16_from_host(val: vec2f) -> vec2f {
  return val * 32.0;
}

fn xe_scale_k_16_16_to_host4(val: vec4f) -> vec4f {
  return val * 0.03125;
}

fn xe_scale_k_16_16_from_host4(val: vec4f) -> vec4f {
  return val * 32.0;
}


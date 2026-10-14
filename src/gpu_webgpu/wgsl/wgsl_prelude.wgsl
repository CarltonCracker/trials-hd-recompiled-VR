// Helpers shared by every translated Xenos shader (wgsl_shader_translator).
// Semantics follow the SDK's ShaderInterpreter and SpirvShaderTranslator.

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
  var r = m.x + m.y;
  if (count >= 3u) { r += m.z; }
  if (count >= 4u) { r += m.w; }
  return r;
}

// max/min with the operand order of the hardware: NaN in a selects b.
fn xe_max(a: f32, b: f32) -> f32 { return select(b, a, a >= b); }
fn xe_min(a: f32, b: f32) -> f32 { return select(b, a, a < b); }
fn xe_max4v(a: vec4f, b: vec4f) -> vec4f { return select(b, a, a >= b); }
fn xe_min4v(a: vec4f, b: vec4f) -> vec4f { return select(b, a, a < b); }

fn xe_max4(a: vec4f) -> f32 {
  if (a.x >= a.y && a.x >= a.z && a.x >= a.w) { return a.x; }
  if (a.y >= a.z && a.y >= a.w) { return a.y; }
  if (a.z >= a.w) { return a.z; }
  return a.w;
}

const xe_flt_max: f32 = 3.40282346638528859812e+38;

fn xe_is_inf(x: f32) -> bool {
  return (bitcast<u32>(x) & 0x7FFFFFFFu) == 0x7F800000u;
}

// Clamping variants of the reciprocal and logarithm.
fn xe_clamp_inf_to_max(x: f32) -> f32 {
  return select(x, select(xe_flt_max, -xe_flt_max, x < 0.0), xe_is_inf(x));
}
fn xe_clamp_inf_to_zero(x: f32) -> f32 {
  return select(x, select(0.0, -0.0, x < 0.0), xe_is_inf(x));
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
  return i32(floor(clamp(x, -256.0, 255.0) + select(0.0, 0.5, round)));
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

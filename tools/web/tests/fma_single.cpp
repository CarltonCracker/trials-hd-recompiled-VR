// Independent exactness/performance checks for W-L16; no game data.
#undef NDEBUG
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <rex/ppc/web_fma.h>

namespace {
uint64_t state = 0xA36E8D5B72C490F1ull;
uint64_t Random() {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

double FloatRandom() { return double(std::bit_cast<float>(uint32_t(Random()))); }
double DoubleRandom() { return std::bit_cast<double>(Random()); }

[[gnu::noinline]] double Reference(double a, double b, double c) {
  return std::fma(a, b, c);
}
[[gnu::noinline]] double Fast(double a, double b, double c) {
  return rex::ppc::FmaSingle(a, b, c);
}

uint64_t comparisons = 0;
void Equal(double actual, double expected, double a, double b, double c) {
  ++comparisons;
  if (std::bit_cast<uint64_t>(actual) != std::bit_cast<uint64_t>(expected)) {
    std::fprintf(stderr, "FMA mismatch: a=%.17g b=%.17g c=%.17g actual=%016llx expected=%016llx\n",
                 a, b, c, (unsigned long long)std::bit_cast<uint64_t>(actual),
                 (unsigned long long)std::bit_cast<uint64_t>(expected));
    assert(false);
  }
}

void Check(double a, double b, double c) {
  for (int subtract = 0; subtract < 2; ++subtract) {
    double z = subtract ? -c : c;
    double reference = Reference(a, b, z);
    double fast = Fast(a, b, z);
    Equal(fast, reference, a, b, z);  // Stronger than the final float result.
    Equal(double(float(fast)), double(float(reference)), a, b, z);
    Equal(double(float(-fast)), double(float(-reference)), a, b, z);
  }
}

volatile double checksum = 0;
using Operation = double (*)(double, double, double);
double Benchmark(Operation operation, const std::array<double, 1024>& values) {
  auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < 1000000; ++i) {
    double value = operation(values[i & 1023], values[(i + 19) & 1023], values[(i + 43) & 1023]);
    checksum = checksum + value;
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}  // namespace

int main() {
  const double inf = std::numeric_limits<double>::infinity();
  const std::array<double, 23> edges = {
      0.0, -0.0, 1.0, -1.0, 0.5, -0.5,
      double(std::numeric_limits<float>::max()), -double(std::numeric_limits<float>::max()),
      double(std::numeric_limits<float>::min()), double(std::numeric_limits<float>::denorm_min()),
      std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
      std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(),
      inf, -inf, std::bit_cast<double>(0x7FF8000000001234ull),
      std::bit_cast<double>(0x7FF0000000000001ull), std::bit_cast<double>(0xFFF8000000005678ull),
      1.0 + std::ldexp(1.0, -27), 1.0 - std::ldexp(1.0, -27),
      std::nextafter(1.0, inf), std::nextafter(1.0, -inf),
  };
  for (double a : edges) for (double b : edges) for (double c : edges) Check(a, b, c);
  // Non-float operands must fall back: separate multiply/add would return zero.
  Check(1.0 + std::ldexp(1.0, -27), 1.0 - std::ldexp(1.0, -27), -1.0);

  for (unsigned i = 0; i < 2000000; ++i) {
    double a = FloatRandom(), b = FloatRandom(), c;
    switch (i & 3) {
      case 0: c = FloatRandom(); break;
      case 1: c = DoubleRandom(); break;
      case 2: a = DoubleRandom(); b = DoubleRandom(); c = DoubleRandom(); break;
      default: c = std::nextafter(-(a * b), (i & 4) ? inf : -inf); break;
    }
    Check(a, b, c);
  }
  std::printf("FMA exactness PASSED: %llu bit comparisons (all four generated forms)\n",
              (unsigned long long)comparisons);

  std::array<double, 1024> values;
  for (auto& value : values) {
    value = double(std::bit_cast<float>(uint32_t(Random() & 0x807FFFFF) | 0x3F000000));
  }
  double reference_ms = inf, fast_ms = inf;
  for (int repeat = 0; repeat < 3; ++repeat) {
    reference_ms = std::min(reference_ms, Benchmark(Reference, values));
    fast_ms = std::min(fast_ms, Benchmark(Fast, values));
  }
  std::printf("FMA benchmark: reference %.3f ms, guarded %.3f ms, speedup %.2fx\n",
              reference_ms, fast_ms, reference_ms / fast_ms);
#if defined(__wasm__)
  assert(fast_ms < reference_ms);
#endif
}

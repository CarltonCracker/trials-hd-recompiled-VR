# Synthetic WebGPU checks (Linux)

Builds separately from the game so parallel agents do not share a Ninja directory
or overwrite staged plugins. Requires an already-built SDK and the pinned Dawn
prefix; no game files are needed for these tests.

```sh
PATH="$PWD/.venv/bin:$PATH" cmake -S tests/gpu_webgpu -B out/webgpu-tests -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$PWD/out/dawn-install"
PATH="$PWD/.venv/bin:$PATH" cmake --build out/webgpu-tests -j2
out/webgpu-tests/shared_memory_test 0
out/webgpu-tests/alu_test 0
# Repeat with adapter 1; each executable prints the selected GPU.
```

The default SDK paths refer to `thirdparty/rexglue-sdk` and the existing
`out/build/linux-amd64-relwithdebinfo/rexglue-sdk` generated headers. Override
`REXSDK_DIR`, `REXSDK_LIBRARY_DIR` or `REXSDK_BUILD_DIR` if your SDK build differs.

- **Shared memory:** raw bytes, page caching, physical-alias write faults,
  encoder-ordered updates without forced submissions, GPU-written pages, cache clearing, retry,
  bounds, callback teardown and rejection of insufficient storage limits.
- **ALU:** authored microcode for all 80 defined vector/scalar opcodes, compared
  with the SDK interpreter. Includes saturation, swizzles/modifiers, previous
  scalar state, predicates, a0, zero/denormal/NaN/infinity edges, and actual
  fragment discards checked against the interpreter's kill result.

Floating arithmetic uses a 2e-5 relative/absolute tolerance; NaN and infinity
classification must match. Reserved opcodes are not instructions and are excluded.
These checks do not establish scene parity, performance budgets or Metal parity.

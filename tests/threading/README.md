# POSIX alertable waits

Builds the edited SDK `threading_posix.cpp` into an isolated host-only test.
It does not rebuild or stage the SDK/game libraries, and needs no game data.
Requires the existing SDK libraries and generated headers (paths can be changed
with `REXSDK_DIR`, `REXSDK_LIBRARY_DIR`, `REXSDK_BUILD_DIR`).

```sh
PATH="$PWD/.venv/bin:$PATH" cmake -S tests/threading -B out/alertable-wait-test \
  -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
PATH="$PWD/.venv/bin:$PATH" cmake --build out/alertable-wait-test -j2
out/alertable-wait-test/alertable_wait_test
```

Checks pending-before-wait callbacks, indefinite single/multiple waits,
signal-and-wait, nested alertable state, FIFO/once-only dispatch across 1,000
queue/registration races, wait-all state preservation, actual condition signals,
zero and finite timeouts, idle thread CPU and short-deadline CPU spin.

The test joins every created thread. Timing limits allow scheduler noise;
printed CPU/latency values are measurements, not cross-machine guarantees.
Game audio/file-IO behavior and process-wide CPU need separate live checks.

#!/usr/bin/env python3
"""Run the native probe suite for W-S06, W-S07, and W-S08.

Executes headless test scripts with libnative_probe.so interposed to measure:
- Startup time to first guest swap
- Host stack depth of guest threads (mincore / VmStk)
- Thread creation counts and peak concurrent threads
- Prove MMIO completeness (no unhandled 0x7F000000..0x7FFFFFFF faults)
- Check for any fiber usage (Fiber::Create, RegisterFiber, rexcrt_*)
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
LOCK_FILE = ROOT / "out/native-web-check.lock"
PROBE_SO = ROOT / "out/web-spikes/libnative_probe.so"
VENV_BIN = ROOT / ".venv/bin"

SCRIPTS = [
    {
        "name": "main_menu_nav",
        "script": "tests/headless/main_menu_nav.txt",
        "duration": 45,
        "args": ["--gpu_plugin=webgpu", "--webgpu_headless=true", "--webgpu_adapter=1"],
    },
    {
        "name": "ride_middle_name_danger",
        "script": "tests/headless/ride_middle_name_danger.txt",
        "duration": 80,
        "args": [
            "--gpu_plugin=webgpu",
            "--webgpu_headless=true",
            "--webgpu_adapter=1",
            "--present_fps_limit=60",
        ],
    },
    {
        "name": "settings_menu",
        "script": "tests/headless/settings_menu.txt",
        "duration": 40,
        "args": ["--gpu_plugin=webgpu", "--webgpu_headless=true", "--webgpu_adapter=1"],
    },
    {
        "name": "credits",
        "script": "tests/headless/credits.txt",
        "duration": 45,
        "args": ["--gpu_plugin=webgpu", "--webgpu_headless=true", "--webgpu_adapter=1"],
    },
]


def run_test(test):
    name = test["name"]
    json_path = ROOT / f"out/web-spikes/probe_{name}.json"
    if json_path.exists():
        json_path.unlink()

    env = dict(os.environ)
    env["PATH"] = f"{VENV_BIN}:{env.get('PATH', '')}"
    env["LD_PRELOAD"] = str(PROBE_SO)
    env["PROBE_OUTPUT_JSON"] = str(json_path)
    env["INPUT"] = str(ROOT / test["script"])
    env["LOG_LEVEL"] = "info"

    cmd = [
        "flock",
        str(LOCK_FILE),
        str(ROOT / "tools/headless_run.sh"),
        str(test["duration"]),
    ] + test["args"]

    print(f"\n========================================================")
    print(f"Running probe test: {name} ({test['duration']}s)")
    print(f"Command: {' '.join(cmd)}")
    print(f"========================================================")
    t0 = time.time()
    res = subprocess.run(cmd, cwd=str(ROOT), env=env, capture_output=True, text=True)
    elapsed = time.time() - t0
    print(f"Completed in {elapsed:.1f}s, exit code: {res.returncode}")

    if not json_path.exists():
        print(f"ERROR: {json_path} was not generated!")
        print("Stdout:\n", res.stdout[-1000:] if res.stdout else "")
        print("Stderr:\n", res.stderr[-1000:] if res.stderr else "")
        return None

    with open(json_path) as f:
        data = json.load(f)
    data["name"] = name
    data["elapsed_s"] = elapsed
    return data


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else None
    results = {}
    for test in SCRIPTS:
        if target and test["name"] != target:
            continue
        res = run_test(test)
        if res is None:
            print(f"Test {test['name']} FAILED!")
            sys.exit(1)
        results[test["name"]] = res

    # Summary table
    print("\n" + "=" * 80)
    print("                      PROBE SUITE SUMMARY")
    print("=" * 80)
    print(f"{'Test':<26} {'1st Swap (ms)':<14} {'Swaps':<8} {'Threads':<9} {'Peak':<6} {'MaxDepth(KiB)':<14} {'Fibers':<8}")
    print("-" * 80)
    for name, r in results.items():
        print(f"{name:<26} {r['first_swap_ms']:<14.2f} {r['total_swaps']:<8} "
              f"{r['total_threads_created']:<9} {r['peak_concurrent_threads']:<6} "
              f"{r['max_host_stack_depth_guest_kib']:<14.2f} {r['fiber_calls']:<8}")
    print("=" * 80)

    # Combined summary file
    summary_path = ROOT / "out/web-spikes/probe_summary.json"
    with open(summary_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nSummary written to {summary_path}")


if __name__ == "__main__":
    main()

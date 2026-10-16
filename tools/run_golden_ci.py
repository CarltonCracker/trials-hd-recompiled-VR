#!/usr/bin/env python3
"""
run_golden_ci.py - Golden-scene CI script for renderer parity verification (S-32).

Runs headless captures or compares existing captures between reference plugin (xenos/Vulkan)
and test plugin (webgpu), evaluates PSNR and SSIM against per-scene thresholds from golden.toml,
generates side-by-side diff images, and prints a formatted Markdown pass/fail table.

Usage:
  tools/run_golden_ci.py [--config CONFIG] [--ref-dir DIR] [--test-dir DIR] [--run] [--self-test]
"""

import argparse
import os
import subprocess
import sys
import time

# Auto-reexec inside repo .venv if numpy or PIL are missing from global environment.
_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_venv_python = os.path.join(_root, ".venv", "bin", "python3")
if os.path.exists(_venv_python) and sys.executable != _venv_python:
    try:
        import numpy
        from PIL import Image
    except ImportError:
        os.execv(_venv_python, [_venv_python] + sys.argv)

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("Error: numpy and Pillow are required. Activate the repo .venv (source .venv/bin/activate)", file=sys.stderr)
    sys.exit(1)

# Import image comparison functions from compare_captures.py
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_captures import compute_psnr, compute_ssim, create_side_by_side_diff


def load_image(path):
    """Load image as PIL Image and float32 numpy array [0, 255]."""
    img = Image.open(path).convert("RGB")
    arr = np.array(img, dtype=np.float32)
    return img, arr


def run_scene_headless(script_path, plugin, output_dir, timeout=45):
    """Run a headless capture script using tools/headless_run.sh with specified GPU plugin."""
    cmd = [
        "bash", os.path.join(_root, "tools", "headless_run.sh"),
        str(timeout),
        f"--gpu_plugin={plugin}",
    ]
    env = os.environ.copy()
    env["INPUT"] = script_path
    env["TRIALS_HD_EXE"] = os.path.join(_root, "out", "build", "linux-amd64-relwithdebinfo", "trials_hd")
    print(f"  [RUN] {plugin}: {script_path} (timeout={timeout}s)...", flush=True)
    res = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return res.returncode == 0


def evaluate_scene(name, cfg, ref_dir, test_dir, diff_dir=None):
    """
    Compare reference vs test captures for a golden scene.
    Returns dict with evaluation metrics, thresholds, and status.
    """
    capture_name = cfg["capture"]
    min_psnr = float(cfg.get("min_psnr", 30.0))
    min_ssim = float(cfg.get("min_ssim", 0.90))

    ref_path = os.path.join(ref_dir, f"{capture_name}.png")
    test_path = os.path.join(test_dir, f"{capture_name}.png")

    if not os.path.exists(ref_path):
        return {
            "scene": name, "capture": f"{capture_name}.png",
            "psnr": 0.0, "min_psnr": min_psnr,
            "ssim": 0.0, "min_ssim": min_ssim,
            "status": "FAIL", "reason": f"Missing ref: {ref_path}",
            "diff_path": "",
        }

    if not os.path.exists(test_path):
        return {
            "scene": name, "capture": f"{capture_name}.png",
            "psnr": 0.0, "min_psnr": min_psnr,
            "ssim": 0.0, "min_ssim": min_ssim,
            "status": "FAIL", "reason": f"Missing test: {test_path}",
            "diff_path": "",
        }

    ref_img, ref_arr = load_image(ref_path)
    test_img, test_arr = load_image(test_path)

    # Check dimension match
    if ref_arr.shape != test_arr.shape:
        return {
            "scene": name, "capture": f"{capture_name}.png",
            "psnr": 0.0, "min_psnr": min_psnr,
            "ssim": 0.0, "min_ssim": min_ssim,
            "status": "FAIL",
            "reason": f"Size mismatch: {ref_arr.shape} vs {test_arr.shape}",
            "diff_path": "",
        }

    psnr = compute_psnr(ref_arr, test_arr)
    ssim = compute_ssim(ref_arr, test_arr)

    psnr_ok = (psnr >= min_psnr)
    ssim_ok = (ssim >= min_ssim)
    passed = psnr_ok and ssim_ok

    diff_path = ""
    if diff_dir:
        os.makedirs(diff_dir, exist_ok=True)
        diff_file = os.path.join(diff_dir, f"diff_{capture_name}.png")
        diff_arr = np.abs(ref_arr - test_arr)
        diff_img = create_side_by_side_diff(ref_img, test_img, diff_arr, psnr, ssim, title=name)
        diff_img.save(diff_file)
        diff_path = diff_file

    return {
        "scene": name,
        "capture": f"{capture_name}.png",
        "psnr": psnr,
        "min_psnr": min_psnr,
        "ssim": ssim,
        "min_ssim": min_ssim,
        "status": "PASS" if passed else "FAIL",
        "reason": "" if passed else ("Low PSNR" if not psnr_ok else "Low SSIM"),
        "diff_path": diff_path,
    }


def format_markdown_table(results):
    """Format evaluation results into a Markdown table."""
    lines = []
    lines.append("| Scene | Capture | PSNR (dB) | Min PSNR | SSIM | Min SSIM | Status | Details |")
    lines.append("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :--- |")

    for r in results:
        psnr_str = f"{r['psnr']:.1f}" if r['psnr'] != float("inf") else "inf"
        min_psnr_str = f"{r['min_psnr']:.1f}"
        ssim_str = f"{r['ssim']:.4f}"
        min_ssim_str = f"{r['min_ssim']:.4f}"
        status_str = f"**{r['status']}**" if r['status'] == "PASS" else f"<span style='color:red'>**{r['status']}**</span>"
        details = r['reason'] if r['reason'] else "OK"
        if r['diff_path']:
            details += f" ([diff]({r['diff_path']}))"
        lines.append(f"| `{r['scene']}` | `{r['capture']}` | {psnr_str} | {min_psnr_str} | {ssim_str} | {min_ssim_str} | {status_str} | {details} |")

    return "\n".join(lines)


def run_ci(config_path, ref_dir, test_dir, diff_dir=None, execute=False, ref_plugin="xenos", test_plugin="webgpu"):
    """Run golden-scene CI verification pipeline."""
    with open(config_path, "rb") as f:
        config = tomllib.load(f)

    scenes = config.get("scenes", {})
    if not scenes:
        print(f"Error: No scenes found in {config_path}", file=sys.stderr)
        return False

    print(f"Loaded {len(scenes)} golden scenes from {config_path}")

    # Optional live headless execution
    if execute:
        print("Executing headless scripts...")
        os.makedirs(ref_dir, exist_ok=True)
        os.makedirs(test_dir, exist_ok=True)
        for name, cfg in scenes.items():
            script = cfg.get("script")
            if script and os.path.exists(os.path.join(_root, script)):
                run_scene_headless(script, ref_plugin, ref_dir)
                run_scene_headless(script, test_plugin, test_dir)

    # Evaluate all scenes
    results = []
    total_passed = 0
    for name, cfg in scenes.items():
        res = evaluate_scene(name, cfg, ref_dir, test_dir, diff_dir)
        results.append(res)
        if res["status"] == "PASS":
            total_passed += 1

    table = format_markdown_table(results)
    print("\n### Golden-Scene Parity Report\n")
    print(table)
    print(f"\n**Summary:** {total_passed}/{len(results)} scenes passed thresholds.")

    return total_passed == len(results)


def self_test():
    """Run self-test on test images to verify table output and threshold logic."""
    print("Running run_golden_ci.py self-test...")
    from PIL import ImageDraw

    test_tmp = "/tmp/golden_ci_selftest"
    ref_d = os.path.join(test_tmp, "ref")
    test_d = os.path.join(test_tmp, "test")
    diff_d = os.path.join(test_tmp, "diff")
    os.makedirs(ref_d, exist_ok=True)
    os.makedirs(test_d, exist_ok=True)

    # Create synthetic test images
    img1 = Image.new("RGBA", (128, 128), (50, 100, 150, 255))
    d1 = ImageDraw.Draw(img1)
    d1.rectangle([20, 20, 80, 80], fill=(200, 50, 50, 255))
    img1.save(os.path.join(ref_d, "test_cap.png"))

    # Slight modification (should pass with reasonable thresholds)
    img2 = img1.copy()
    d2 = ImageDraw.Draw(img2)
    d2.point([50, 50], fill=(202, 52, 52, 255))
    img2.save(os.path.join(test_d, "test_cap.png"))

    cfg = {
        "scenes": {
            "synthetic_pass": {
                "capture": "test_cap",
                "min_psnr": 30.0,
                "min_ssim": 0.90,
            },
            "synthetic_strict_fail": {
                "capture": "test_cap",
                "min_psnr": 100.0,  # unreachable on non-identical
                "min_ssim": 0.9999,
            }
        }
    }

    res1 = evaluate_scene("synthetic_pass", cfg["scenes"]["synthetic_pass"], ref_d, test_d, diff_d)
    assert res1["status"] == "PASS", f"Expected PASS, got {res1['status']}"

    res2 = evaluate_scene("synthetic_strict_fail", cfg["scenes"]["synthetic_strict_fail"], ref_d, test_d, diff_d)
    assert res2["status"] == "FAIL", f"Expected FAIL, got {res2['status']}"

    table = format_markdown_table([res1, res2])
    print(table)
    print("\nSelf-test PASSED successfully!")
    return True


def main():
    parser = argparse.ArgumentParser(description="Golden-scene CI verification script")
    parser.add_argument("--config", default=os.path.join(_root, "tests", "headless", "golden.toml"),
                        help="Path to golden.toml configuration")
    parser.add_argument("--ref-dir", default=os.path.join(_root, "out", "run"),
                        help="Directory containing reference captures")
    parser.add_argument("--test-dir", default=os.path.join(_root, "out", "run"),
                        help="Directory containing test captures")
    parser.add_argument("--diff-dir", default=os.path.join(_root, "out", "golden_diffs"),
                        help="Directory to save diff images")
    parser.add_argument("--run", action="store_true", help="Execute headless scripts before evaluation")
    parser.add_argument("--ref-plugin", default="xenos", help="Reference plugin (xenos/vulkan)")
    parser.add_argument("--test-plugin", default="webgpu", help="Test plugin (webgpu)")
    parser.add_argument("--self-test", action="store_true", help="Run self-test")

    args = parser.parse_args()

    if args.self_test:
        success = self_test()
        sys.exit(0 if success else 1)

    success = run_ci(
        config_path=args.config,
        ref_dir=args.ref_dir,
        test_dir=args.test_dir,
        diff_dir=args.diff_dir,
        execute=args.run,
        ref_plugin=args.ref_plugin,
        test_plugin=args.test_plugin,
    )

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()

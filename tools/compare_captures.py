#!/usr/bin/env python3
"""
compare_captures.py - Image comparison tool computing PSNR and SSIM between PNG pairs.

Features:
- Computes PSNR (Peak Signal-to-Noise Ratio) in dB.
- Computes SSIM (Structural Similarity Index) [0..1].
- Per-scene thresholds from a TOML configuration file (e.g. tests/headless/golden.toml).
- Side-by-side diff image generation (Reference | Test | Amplified Difference).
- Non-zero exit code if metrics do not meet threshold requirements.
- Self-test mode (--self-test) on identical and different images.
"""

import argparse
import math
import os
import sys

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib

try:
    import numpy as np
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Error: numpy and Pillow are required. Activate the repo .venv (source .venv/bin/activate)", file=sys.stderr)
    sys.exit(1)


def compute_psnr(img1_arr, img2_arr):
    """
    Compute Peak Signal-to-Noise Ratio (PSNR) between two float32 images [0, 255].
    Returns float ('inf' if MSE is 0).
    """
    mse = np.mean((img1_arr - img2_arr) ** 2)
    if mse == 0.0:
        return float("inf")
    max_pixel = 255.0
    return 20.0 * math.log10(max_pixel / math.sqrt(mse))


def compute_ssim_channel(arr1, arr2, win_size=11, sigma=1.5):
    """
    Compute SSIM on a single 2D channel [0, 255].
    Uses standard Wang et al. 11x11 Gaussian window.
    """
    k1, k2, l = 0.01, 0.03, 255.0
    c1 = (k1 * l) ** 2
    c2 = (k2 * l) ** 2

    # 1D Gaussian kernel
    coords = np.arange(win_size) - win_size // 2
    k = np.exp(-0.5 * (coords / sigma) ** 2)
    k /= k.sum()

    def filter2d(a):
        # Separable convolution mode='valid'
        r = np.apply_along_axis(lambda m: np.convolve(m, k, mode="valid"), axis=1, arr=a)
        return np.apply_along_axis(lambda m: np.convolve(m, k, mode="valid"), axis=0, arr=r)

    # If image is smaller than window size, fall back to global mean/variance
    if arr1.shape[0] < win_size or arr1.shape[1] < win_size:
        mu1 = np.mean(arr1)
        mu2 = np.mean(arr2)
        var1 = np.var(arr1)
        var2 = np.var(arr2)
        cov = np.mean((arr1 - mu1) * (arr2 - mu2))
        return float(((2 * mu1 * mu2 + c1) * (2 * cov + c2)) /
                     ((mu1 ** 2 + mu2 ** 2 + c1) * (var1 + var2 + c2)))

    mu1 = filter2d(arr1)
    mu2 = filter2d(arr2)
    mu1_sq = mu1 * mu1
    mu2_sq = mu2 * mu2
    mu1_mu2 = mu1 * mu2

    sigma1_sq = filter2d(arr1 * arr1) - mu1_sq
    sigma2_sq = filter2d(arr2 * arr2) - mu2_sq
    sigma12 = filter2d(arr1 * arr2) - mu1_mu2

    ssim_map = ((2 * mu1_mu2 + c1) * (2 * sigma12 + c2)) / \
               ((mu1_sq + mu2_sq + c1) * (sigma1_sq + sigma2_sq + c2))
    return float(np.mean(ssim_map))


def compute_ssim(img1_arr, img2_arr):
    """Compute mean SSIM across all color channels."""
    if img1_arr.ndim == 2:
        return compute_ssim_channel(img1_arr, img2_arr)
    channels = [compute_ssim_channel(img1_arr[..., c], img2_arr[..., c])
                for c in range(img1_arr.shape[-1])]
    return float(np.mean(channels))


def create_side_by_side_diff(img1, img2, diff_arr, psnr, ssim, title=""):
    """
    Create a 3-panel side-by-side image:
    [ Reference Image | Test Image | Difference (amplified) ]
    """
    w, h = img1.size
    amp_factor = 4.0
    amp_diff = np.clip(np.abs(diff_arr) * amp_factor, 0, 255).astype(np.uint8)
    if amp_diff.ndim == 2:
        diff_img = Image.fromarray(amp_diff, mode="L").convert("RGB")
    else:
        diff_img = Image.fromarray(amp_diff, mode="RGB")

    header_h = 30
    sbs = Image.new("RGB", (w * 3, h + header_h), (20, 20, 20))
    sbs.paste(img1, (0, header_h))
    sbs.paste(img2, (w, header_h))
    sbs.paste(diff_img, (w * 2, header_h))

    draw = ImageDraw.Draw(sbs)
    psnr_str = "inf" if math.isinf(psnr) else f"{psnr:.2f} dB"
    label = f"Ref: (left) | Test: (center) | Diff x{amp_factor:.0f}: (right) -- PSNR: {psnr_str}, SSIM: {ssim:.4f}"
    if title:
        label = f"[{title}] {label}"
    draw.text((10, 8), label, fill=(255, 255, 255))

    return sbs


def compare_image_files(ref_path, test_path, diff_out=None, min_psnr=None, min_ssim=None, title=""):
    """
    Compare two image files.
    Returns (passed, psnr, ssim, message)
    """
    if not os.path.exists(ref_path):
        return False, 0.0, 0.0, f"Reference image missing: {ref_path}"
    if not os.path.exists(test_path):
        return False, 0.0, 0.0, f"Test image missing: {test_path}"

    im_ref = Image.open(ref_path).convert("RGB")
    im_test = Image.open(test_path).convert("RGB")

    if im_ref.size != im_test.size:
        return False, 0.0, 0.0, f"Size mismatch: {im_ref.size} vs {im_test.size}"

    arr_ref = np.array(im_ref, dtype=np.float32)
    arr_test = np.array(im_test, dtype=np.float32)

    psnr = compute_psnr(arr_ref, arr_test)
    ssim = compute_ssim(arr_ref, arr_test)

    passed = True
    reasons = []
    if min_psnr is not None and not math.isinf(psnr) and psnr < min_psnr:
        passed = False
        reasons.append(f"PSNR {psnr:.2f} dB < {min_psnr:.2f} dB")
    if min_ssim is not None and ssim < min_ssim:
        passed = False
        reasons.append(f"SSIM {ssim:.4f} < {min_ssim:.4f}")

    if diff_out:
        os.makedirs(os.path.dirname(os.path.abspath(diff_out)), exist_ok=True)
        diff_sbs = create_side_by_side_diff(im_ref, im_test, arr_ref - arr_test, psnr, ssim, title=title)
        diff_sbs.save(diff_out)

    msg = f"PSNR: {'inf' if math.isinf(psnr) else f'{psnr:.2f} dB'}, SSIM: {ssim:.4f}"
    if reasons:
        msg += f" (FAIL: {', '.join(reasons)})"
    else:
        msg += " (PASS)"

    return passed, psnr, ssim, msg


def run_self_test():
    """Self-test comparing identical images and different images."""
    print("Running compare_captures self-test...")
    import tempfile

    with tempfile.TemporaryDirectory() as tmpdir:
        # 1. Identical images
        img1 = Image.new("RGB", (200, 200), (100, 150, 200))
        path1 = os.path.join(tmpdir, "img1.png")
        img1.save(path1)

        ok, psnr, ssim, msg = compare_image_files(path1, path1, min_psnr=40.0, min_ssim=0.99)
        assert ok, f"Identical comparison should pass: {msg}"
        assert math.isinf(psnr), f"Identical PSNR should be inf: {psnr}"
        assert abs(ssim - 1.0) < 1e-5, f"Identical SSIM should be 1.0: {ssim}"
        print(f"  [PASS] Identical images: {msg}")

        # 2. Different images
        img2 = Image.new("RGB", (200, 200), (120, 130, 220))
        path2 = os.path.join(tmpdir, "img2.png")
        diff_path = os.path.join(tmpdir, "diff.png")
        img2.save(path2)

        ok, psnr, ssim, msg = compare_image_files(path2, path1, diff_out=diff_path, min_psnr=50.0)
        assert not ok, "Different images with high min_psnr should fail"
        assert not math.isinf(psnr), "Different PSNR should not be inf"
        assert os.path.exists(diff_path), "Diff image should have been generated"
        print(f"  [PASS] Different images correctly failed threshold: {msg}")
        print(f"  [PASS] Diff image successfully created: {diff_path}")

    print("Self-test completed successfully.")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Image comparison tool for golden captures")
    parser.add_argument("ref", nargs="?", help="Reference PNG path")
    parser.add_argument("test", nargs="?", help="Test PNG path")
    parser.add_argument("--diff", help="Path to write side-by-side diff PNG")
    parser.add_argument("--min-psnr", type=float, help="Minimum acceptable PSNR in dB")
    parser.add_argument("--min-ssim", type=float, help="Minimum acceptable SSIM (0.0 - 1.0)")
    parser.add_argument("--config", help="Path to golden scenes TOML configuration file")
    parser.add_argument("--ref-dir", default=".", help="Base directory for reference images")
    parser.add_argument("--test-dir", default=".", help="Base directory for test images")
    parser.add_argument("--diff-dir", help="Directory to save diff images for config mode")
    parser.add_argument("--self-test", action="store_true", help="Run self-test on test images")

    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if args.config:
        if not os.path.exists(args.config):
            print(f"Error: config file not found: {args.config}", file=sys.stderr)
            return 1
        with open(args.config, "rb") as f:
            cfg = tomllib.load(f)

        scenes = cfg.get("scenes", {})
        if not scenes:
            print(f"Error: no scenes found in {args.config}", file=sys.stderr)
            return 1

        print(f"Comparing {len(scenes)} scenes from {args.config}...")
        all_passed = True
        for name, entry in scenes.items():
            capture_name = entry.get("capture", name)
            filename = entry.get("filename", f"{capture_name}.png")
            ref_path = os.path.join(args.ref_dir, filename)
            test_path = os.path.join(args.test_dir, filename)
            min_psnr = entry.get("min_psnr")
            min_ssim = entry.get("min_ssim")
            diff_out = os.path.join(args.diff_dir, f"diff_{filename}") if args.diff_dir else None

            ok, psnr, ssim, msg = compare_image_files(
                ref_path, test_path, diff_out=diff_out,
                min_psnr=min_psnr, min_ssim=min_ssim, title=name
            )
            status_str = "PASS" if ok else "FAIL"
            print(f"  [{status_str}] {name:<20}: {msg}")
            if not ok:
                all_passed = False

        return 0 if all_passed else 2

    if not args.ref or not args.test:
        parser.print_help()
        return 1

    ok, psnr, ssim, msg = compare_image_files(
        args.ref, args.test, diff_out=args.diff,
        min_psnr=args.min_psnr, min_ssim=args.min_ssim
    )
    print(msg)
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())

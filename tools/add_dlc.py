#!/usr/bin/env python3
"""Install Trials HD DLC / Addon packages into the assets directory.

Usage:
  python3 tools/add_dlc.py <addon_folder_or_package> [assets_dir]
  add_dlc.sh <addon_folder_or_package> [assets_dir]

Supports:
- Single STFS package file (LIVE/PIRS/CON)
- Addon folder (e.g. "Trials HD - Big Pack (World) (Addon)")
- Top-level directory containing multiple addons (e.g. "game/")
- Extracted DLC folder containing pack.pak
"""

import hashlib
import os
import re
import struct
import sys

# Ensure tools directory is in sys.path to import stfs_extract
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from stfs_extract import Stfs

TRIALS_HD_TITLE_ID = 0x5841095A
CONTENT_TYPE_DLC = 0x00000002


def make_header_data(
    display_name: str,
    package_id: str,
    title_id: int = TRIALS_HD_TITLE_ID,
    content_type: int = CONTENT_TYPE_DLC,
    license_mask: int = 1,
) -> bytes:
    """Generate the 332-byte .header file expected by ReXGlue ContentManager."""
    buf = bytearray(332)
    # device_id = 1 (HDD), content_type = 2 (DLC)
    struct.pack_into(">II", buf, 0x000, 1, content_type)

    # display_name_raw: 128 uint16 (utf-16-be) = 256 bytes
    disp_bytes = display_name.encode("utf-16-be")[:254]
    buf[0x008 : 0x008 + len(disp_bytes)] = disp_bytes

    # file_name_raw: 42 bytes ASCII
    fname_bytes = package_id.encode("ascii", "replace")[:42]
    buf[0x108 : 0x108 + len(fname_bytes)] = fname_bytes

    # xuid (be64) = 0, title_id (be32) = title_id
    struct.pack_into(">QI", buf, 0x138, 0, title_id)

    # license_mask (le32) = license_mask
    struct.pack_into("<I", buf, 0x148, license_mask)

    return bytes(buf)


def is_stfs_file(path: str) -> bool:
    try:
        if os.path.getsize(path) < 0x400:
            return False
        with open(path, "rb") as f:
            magic = f.read(4)
            return magic in (b"LIVE", b"PIRS", b"CON ")
    except Exception:
        return False


def get_stfs_info(path: str):
    """Returns (is_dlc, title_id, content_type, display_name) or None."""
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
            if magic not in (b"LIVE", b"PIRS", b"CON "):
                return None
            f.seek(0x344)
            content_type = struct.unpack(">I", f.read(4))[0]
            f.seek(0x360)
            title_id = struct.unpack(">I", f.read(4))[0]
            f.seek(0x411)
            display_name = (
                f.read(0x80).decode("utf-16-be", "ignore").split("\0")[0]
            )
            is_dlc = (content_type == CONTENT_TYPE_DLC) and (
                title_id in (TRIALS_HD_TITLE_ID, 0x5842095A)
            )
            return (is_dlc, title_id, content_type, display_name)
    except Exception:
        return None


def derive_package_id(file_or_dir_name: str, file_path: str = None) -> str:
    """Ensure a valid 42-character ASCII package ID."""
    name = os.path.basename(file_or_dir_name)
    # Check if name is already a 42-char hex string
    if re.fullmatch(r"[0-9A-Fa-f]{42}", name):
        return name.upper()
    # Or if 40-char hex string (SHA1)
    if re.fullmatch(r"[0-9A-Fa-f]{40}", name):
        return (name + "58").upper()

    # Generate deterministic 42-character hex ID
    h = hashlib.sha1()
    if file_path and os.path.isfile(file_path):
        try:
            with open(file_path, "rb") as f:
                h.update(f.read(0x10000))
        except Exception:
            h.update(name.encode("utf-8"))
    else:
        h.update(name.encode("utf-8"))
    return (h.hexdigest() + "58")[:42].upper()


def install_stfs_package(pkg_path: str, assets_dir: str) -> dict:
    """Extract an STFS package and write its .header file."""
    s = Stfs(pkg_path)

    # License mask
    mask = 0
    for _, bits, _ in s.licenses:
        mask |= bits
    if mask == 0:
        mask = 1

    package_id = derive_package_id(os.path.basename(pkg_path), pkg_path)
    display_name = s.display_name or f"Trials HD DLC ({package_id[:8]})"
    title_id = TRIALS_HD_TITLE_ID

    title_id_str = f"{title_id:08X}"
    content_type_str = f"{CONTENT_TYPE_DLC:08X}"
    xuid_str = "0000000000000000"

    dest_pkg_dir = os.path.join(
        assets_dir,
        "content",
        xuid_str,
        title_id_str,
        content_type_str,
        package_id,
    )
    dest_header_dir = os.path.join(
        assets_dir,
        "content",
        xuid_str,
        title_id_str,
        "Headers",
        content_type_str,
    )
    dest_header_file = os.path.join(dest_header_dir, f"{package_id}.header")

    os.makedirs(dest_pkg_dir, exist_ok=True)
    os.makedirs(dest_header_dir, exist_ok=True)

    extracted_files = 0
    total_bytes = 0
    try:
        for e in s.entries():
            if e["dir"]:
                continue
            dst = os.path.join(dest_pkg_dir, e["path"])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            content = s.read_file(e)
            with open(dst, "wb") as f:
                f.write(content)
            extracted_files += 1
            total_bytes += len(content)

        for e in s.entries():
            if e["dir"]:
                os.makedirs(os.path.join(dest_pkg_dir, e["path"]), exist_ok=True)
    finally:
        s.f.close()

    # Write .header file (332 bytes)
    header_data = make_header_data(
        display_name=display_name,
        package_id=package_id,
        title_id=title_id,
        content_type=CONTENT_TYPE_DLC,
        license_mask=mask,
    )
    with open(dest_header_file, "wb") as f:
        f.write(header_data)

    # Ensure compatibility symlink assets/0000000000000000 -> content/0000000000000000
    compat_link = os.path.join(assets_dir, xuid_str)
    if not os.path.exists(compat_link):
        try:
            os.symlink(os.path.join("content", xuid_str), compat_link)
        except Exception:
            pass

    return {
        "display_name": display_name,
        "package_id": package_id,
        "package_dir": dest_pkg_dir,
        "header_file": dest_header_file,
        "file_count": extracted_files,
        "total_bytes": total_bytes,
        "license_mask": mask,
    }


def install_extracted_dir(src_dir: str, assets_dir: str) -> dict:
    """Install an already-extracted DLC directory."""
    package_id = derive_package_id(os.path.basename(src_dir))
    display_name = f"Trials HD DLC ({os.path.basename(src_dir)})"

    # Try to find display name from pack*.xus
    for root, _, files in os.walk(src_dir):
        for f in files:
            if f.endswith(".xus"):
                if "pack1" in f:
                    display_name = "Trials HD - Big Pack"
                elif "pack3" in f:
                    display_name = "Trials HD - Big Thrills"

    title_id = TRIALS_HD_TITLE_ID
    title_id_str = f"{title_id:08X}"
    content_type_str = f"{CONTENT_TYPE_DLC:08X}"
    xuid_str = "0000000000000000"

    dest_pkg_dir = os.path.join(
        assets_dir,
        "content",
        xuid_str,
        title_id_str,
        content_type_str,
        package_id,
    )
    dest_header_dir = os.path.join(
        assets_dir,
        "content",
        xuid_str,
        title_id_str,
        "Headers",
        content_type_str,
    )
    dest_header_file = os.path.join(dest_header_dir, f"{package_id}.header")

    os.makedirs(dest_pkg_dir, exist_ok=True)
    os.makedirs(dest_header_dir, exist_ok=True)

    extracted_files = 0
    total_bytes = 0
    import shutil

    for root, dirs, files in os.walk(src_dir):
        rel = os.path.relpath(root, src_dir)
        target_sub = (
            dest_pkg_dir if rel == "." else os.path.join(dest_pkg_dir, rel)
        )
        os.makedirs(target_sub, exist_ok=True)
        for f in files:
            src_f = os.path.join(root, f)
            dst_f = os.path.join(target_sub, f)
            shutil.copy2(src_f, dst_f)
            extracted_files += 1
            total_bytes += os.path.getsize(dst_f)

    header_data = make_header_data(
        display_name=display_name,
        package_id=package_id,
        title_id=title_id,
        content_type=CONTENT_TYPE_DLC,
        license_mask=1,
    )
    with open(dest_header_file, "wb") as f:
        f.write(header_data)

    compat_link = os.path.join(assets_dir, xuid_str)
    if not os.path.exists(compat_link):
        try:
            os.symlink(os.path.join("content", xuid_str), compat_link)
        except Exception:
            pass

    return {
        "display_name": display_name,
        "package_id": package_id,
        "package_dir": dest_pkg_dir,
        "header_file": dest_header_file,
        "file_count": extracted_files,
        "total_bytes": total_bytes,
        "license_mask": 1,
    }


def find_dlc_targets(search_path: str):
    """Find STFS package files or extracted DLC folders."""
    packages = []
    extracted = []

    if os.path.isfile(search_path):
        if is_stfs_file(search_path):
            info = get_stfs_info(search_path)
            # If directly given a file, accept if it's STFS
            packages.append(search_path)
        return packages, extracted

    # If it's a directory, check if it's directly an extracted DLC
    if os.path.isfile(os.path.join(search_path, "pack.pak")):
        extracted.append(search_path)
        return packages, extracted

    # Walk directory to find STFS packages or extracted DLC folders
    for root, dirs, files in os.walk(search_path):
        if "pack.pak" in files and root != search_path:
            # Check if this is an extracted folder inside
            extracted.append(root)
            dirs.clear()  # Don't descend further
            continue
        for f in files:
            full = os.path.join(root, f)
            info = get_stfs_info(full)
            if info and info[0]:  # is_dlc
                packages.append(full)

    return packages, extracted


def main():
    if len(sys.argv) >= 2 and sys.argv[1] in ("-h", "--help"):
        print("Usage: tools/add_dlc.py <addon_folder_or_package> [assets_dir]")
        sys.exit(0)
    if len(sys.argv) < 2:
        print(
            "Usage: tools/add_dlc.py <addon_folder_or_package> [assets_dir]",
            file=sys.stderr,
        )
        sys.exit(1)

    input_path = os.path.abspath(sys.argv[1])
    if not os.path.exists(input_path):
        print(f"Error: Path does not exist: {input_path}", file=sys.stderr)
        sys.exit(1)

    repo_root = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
    assets_dir = (
        os.path.abspath(sys.argv[2])
        if len(sys.argv) > 2
        else os.path.join(repo_root, "assets")
    )

    if not os.path.exists(assets_dir):
        print(
            f"Error: Assets directory does not exist: {assets_dir}",
            file=sys.stderr,
        )
        sys.exit(1)

    packages, extracted = find_dlc_targets(input_path)

    if not packages and not extracted:
        print(
            f"Error: No Trials HD DLC packages or extracted addons found in: {input_path}",
            file=sys.stderr,
        )
        sys.exit(1)

    installed = []

    for pkg in packages:
        print(f"==> Installing STFS DLC package: {pkg}")
        info = install_stfs_package(pkg, assets_dir)
        installed.append(info)
        print(
            f"    -> '{info['display_name']}' [{info['package_id']}] ({info['file_count']} files, {info['total_bytes'] / (1024*1024):.2f} MB)"
        )

    for ext in extracted:
        print(f"==> Installing extracted DLC folder: {ext}")
        info = install_extracted_dir(ext, assets_dir)
        installed.append(info)
        print(
            f"    -> '{info['display_name']}' [{info['package_id']}] ({info['file_count']} files, {info['total_bytes'] / (1024*1024):.2f} MB)"
        )

    print(
        f"\nSuccessfully installed {len(installed)} DLC package(s) into {assets_dir}/content/"
    )
    for item in installed:
        print(f"  * {item['display_name']}")
        print(f"    Package: {item['package_dir']}")
        print(f"    Header:  {item['header_file']}")


if __name__ == "__main__":
    main()

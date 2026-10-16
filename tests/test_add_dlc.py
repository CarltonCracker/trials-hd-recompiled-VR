#!/usr/bin/env python3
"""Unit tests for add_dlc.py and DLC packaging."""

import os
import shutil
import struct
import sys
import tempfile
import unittest

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT_DIR, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from add_dlc import (
    TRIALS_HD_TITLE_ID,
    CONTENT_TYPE_DLC,
    make_header_data,
    derive_package_id,
    find_dlc_targets,
    install_stfs_package,
)


class TestAddDlc(unittest.TestCase):
    def test_make_header_data(self):
        name = "Trials HD - Big Pack"
        pkg_id = "B1A493BA359C56825FB0BD67F37931CEFA71695958"
        hdr = make_header_data(name, pkg_id, TRIALS_HD_TITLE_ID, CONTENT_TYPE_DLC, 1)

        self.assertEqual(len(hdr), 332)

        # Check device_id and content_type
        dev_id, ctype = struct.unpack(">II", hdr[:8])
        self.assertEqual(dev_id, 1)
        self.assertEqual(ctype, CONTENT_TYPE_DLC)

        # Check display name
        disp = hdr[8:264].decode("utf-16-be").split("\0")[0]
        self.assertEqual(disp, name)

        # Check package id / filename
        fname = hdr[264:306].decode("ascii").split("\0")[0]
        self.assertEqual(fname, pkg_id)

        # Check xuid and title_id
        xuid, tid = struct.unpack(">QI", hdr[312:324])
        self.assertEqual(xuid, 0)
        self.assertEqual(tid, TRIALS_HD_TITLE_ID)

        # Check license mask
        lic = struct.unpack("<I", hdr[328:332])[0]
        self.assertEqual(lic, 1)

    def test_derive_package_id(self):
        pkg_id = "B1A493BA359C56825FB0BD67F37931CEFA71695958"
        self.assertEqual(derive_package_id(pkg_id), pkg_id)
        self.assertEqual(len(derive_package_id("custom_pack")), 42)

    def test_find_dlc_targets_in_game_folder(self):
        game_dir = os.path.join(ROOT_DIR, "game")
        if not os.path.isdir(game_dir):
            self.skipTest("game/ directory not found")

        packages, extracted = find_dlc_targets(game_dir)
        # Should find Big Pack and Big Thrills
        self.assertGreaterEqual(len(packages), 2)

        names = [os.path.basename(p) for p in packages]
        self.assertIn("B1A493BA359C56825FB0BD67F37931CEFA71695958", names)
        self.assertIn("C149E88B0A92946B58022D1B063F2911B60748C258", names)

    def test_install_into_temp_assets(self):
        game_dir = os.path.join(ROOT_DIR, "game")
        if not os.path.isdir(game_dir):
            self.skipTest("game/ directory not found")

        temp_dir = tempfile.mkdtemp(prefix="trials_test_assets_")
        try:
            packages, _ = find_dlc_targets(game_dir)
            self.assertTrue(len(packages) >= 1)

            # Test installing one package
            result = install_stfs_package(packages[0], temp_dir)
            self.assertIn("pack.pak", os.listdir(result["package_dir"]))
            self.assertTrue(os.path.isfile(result["header_file"]))
            self.assertEqual(os.path.getsize(result["header_file"]), 332)
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()

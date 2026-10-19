#!/usr/bin/env python3
"""The game installer (src/installer/game_installer.cpp) on synthetic packages.

Usage: tests/installer/test_installer.py <installer_cli binary>

Packs a stand-in game and DLC with make_stfs.py, installs them with the C++
installer and checks the result against tools/add_dlc.py (the DLC layout, IDs
and headers the game already reads), the xex version check and the
interrupted-install guard.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from add_dlc import install_stfs_package, install_extracted_dir  # noqa: E402
sys.path.insert(0, str(HERE))
import make_stfs  # noqa: E402

CLI = sys.argv[1:]
if not CLI:
    raise SystemExit(f'Usage: {sys.argv[0]} <installer_cli binary>')

# Must hash to TRIALSHD_BUILD_XEX_SHA1 in tests/installer/trials_hd_build_info.h.
TEST_XEX = b'trials hd installer test xex\n'
BIG_PACK_ID = 'B1A493BA359C56825FB0BD67F37931CEFA71695958'


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def pack(folder, package, *args):
    package.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(HERE / 'make_stfs.py'), str(folder), str(package), *args],
                   check=True, capture_output=True)


def tree(root):
    return {str(p.relative_to(root)): p.read_bytes()
            for p in sorted(root.rglob('*')) if p.is_file()}


class InstallerTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        game = self.root / 'src/game'
        write(game / 'default.xex', TEST_XEX)
        write(game / 'data/data.pak', os.urandom(3 * 4096 + 17))
        write(game / 'data/data_menu.pak', os.urandom(5000))
        write(game / 'ArcadeInfo.xml', b'<arcade/>')
        self.game = game
        dlc = self.root / 'src/bigpack'
        write(dlc / 'pack.pak', os.urandom(9000))
        write(dlc / 'pack1.xus', b'strings')
        write(dlc / 'en-us/strings.bin', b'en')
        self.dlc = dlc
        # A console content folder: <xuid>/<title>/<content type>/<package>.
        self.content = self.root / 'Content/0000000000000000/5841095A'
        self.game_package = self.content / '000D0000/0123456789ABCDEF0123456789ABCDEF01234567'
        pack(game, self.game_package, '--license', '0xFFFFFFFF')
        self.dlc_package = self.content / f'00000002/{BIG_PACK_ID}'
        pack(dlc, self.dlc_package, '--content-type', '0x2', '--name', 'Trials HD - Big Pack')

    def cli(self, *args):
        result = subprocess.run(CLI + [str(a) for a in args], capture_output=True, text=True,
                                timeout=60)
        return result.returncode, result.stdout.splitlines()

    def test_scan_finds_game_then_dlc(self):
        code, lines = self.cli('scan', self.root / 'Content')
        self.assertEqual(code, 0)
        self.assertEqual([line.split()[1] for line in lines if line.startswith('source')],
                         ['game', 'dlc-package'])
        self.assertIn(BIG_PACK_ID, lines[1])

    def test_install_matches_add_dlc(self):
        assets = self.root / 'out/assets'
        code, lines = self.cli('install', self.root / 'Content', assets)
        self.assertEqual(code, 0, lines)
        self.assertFalse((self.root / 'out/assets.installing').exists())
        installed = tree(assets)
        for name, data in tree(self.game).items():
            self.assertEqual(installed[name], data, name)
        self.assertIn('license_mask = 0xFFFFFFFF', (assets / 'package_license.toml').read_text())

        # tools/add_dlc.py into another folder: same files, same header bytes.
        reference = self.root / 'reference'
        reference.mkdir()
        install_stfs_package(str(self.dlc_package), str(reference))
        expected = {k: v for k, v in tree(reference).items() if k.startswith('content/')}
        self.assertTrue(expected)
        for name, data in expected.items():
            self.assertEqual(installed.get(name), data, name)

    def test_extracted_dlc_folder_matches_add_dlc(self):
        assets = self.root / 'assets'
        write(assets / 'default.xex', TEST_XEX)
        code, lines = self.cli('install', self.dlc, assets)
        self.assertEqual(code, 0, lines)
        reference = self.root / 'reference'
        reference.mkdir()
        install_extracted_dir(str(self.dlc), str(reference))
        expected = {k: v for k, v in tree(reference).items() if k.startswith('content/')}
        self.assertEqual({k: v for k, v in tree(assets).items() if k.startswith('content/')},
                         expected)

    def test_unnamed_dlc_package_gets_stable_id(self):
        renamed = self.root / 'downloads/big pack.bin'
        renamed.parent.mkdir()
        renamed.write_bytes(self.dlc_package.read_bytes())
        _, first = self.cli('scan', renamed)
        _, second = self.cli('scan', renamed)
        self.assertEqual(first, second)
        package_id = first[0].split()[2]
        self.assertEqual(len(package_id), 42)
        reference = self.root / 'reference'
        reference.mkdir()
        self.assertEqual(install_stfs_package(str(renamed), str(reference))['package_id'],
                         package_id)

    def test_other_xex_version_is_refused(self):
        write(self.game / 'default.xex', b'some other build\n')
        other = self.root / 'other/000D0000/0123456789ABCDEF0123456789ABCDEF01234567'
        pack(self.game, other)
        code, lines = self.cli('scan', other)
        self.assertEqual(code, 0)
        self.assertFalse([line for line in lines if line.startswith('source')])
        self.assertTrue(any('different version' in line for line in lines), lines)

    def test_not_a_package(self):
        junk = self.root / 'junk.bin'
        junk.write_bytes(b'\0' * 0x10000)
        _, lines = self.cli('scan', junk)
        self.assertTrue(any(line.startswith('problem') for line in lines), lines)

    def test_failed_first_install_leaves_nothing(self):
        # Break the block chain inside data/data.pak (blocks 2-5: after the file
        # table and ArcadeInfo.xml); the scan still passes, extracting fails.
        broken = self.root / 'broken/0123456789ABCDEF0123456789ABCDEF01234567'
        broken.parent.mkdir()
        data = bytearray(self.game_package.read_bytes())
        record = make_stfs.hash_block_offset(3) + 3 * 0x18 + 0x15
        data[record:record + 3] = b'\xff\xff\xfe'
        broken.write_bytes(data)
        assets = self.root / 'out/assets'
        code, lines = self.cli('install', broken, assets)
        self.assertNotEqual(code, 0, lines)
        self.assertFalse(assets.exists())
        self.assertFalse((self.root / 'out/assets.installing').exists())

    def test_dlc_into_existing_install(self):
        assets = self.root / 'assets'
        code, _ = self.cli('install', self.game_package, assets)
        self.assertEqual(code, 0)
        before = tree(assets)
        code, lines = self.cli('install', self.dlc_package, assets)
        self.assertEqual(code, 0, lines)
        after = tree(assets)
        for name, data in before.items():
            self.assertEqual(after[name], data)
        self.assertIn(f'content/0000000000000000/5841095A/Headers/00000002/{BIG_PACK_ID}.header',
                      after)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])

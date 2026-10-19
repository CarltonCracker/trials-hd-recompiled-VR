#!/usr/bin/env python3
"""Synthetic STFS boundary checks: pass the native CLI, or node + wasm JS CLI."""
from pathlib import Path
import resource
import signal
import struct
import subprocess
import sys
import tempfile
import unittest

COMMAND = sys.argv[1:]
if not COMMAND:
    raise SystemExit(f'Usage: {sys.argv[0]} <extractor command...>')


def package(entries=None, *, table_count=1, table_next=0xffffff, data_next=0xffffff):
    entries = entries if entries is not None else [('payload.bin', False, 0xffff, 4)]
    data = bytearray(0xd000)
    data[:4] = b'LIVE'
    struct.pack_into('>I', data, 0x340, 0xa000)
    struct.pack_into('>I', data, 0x344, 0x000d0000)
    struct.pack_into('>QII', data, 0x22c, 1, 1, 0)
    data[0x37b] = 1
    struct.pack_into('<H', data, 0x37c, table_count)
    struct.pack_into('>I', data, 0x395, 2)
    data[0xa015:0xa018] = table_next.to_bytes(3, 'big')
    data[0xa02d:0xa030] = data_next.to_bytes(3, 'big')
    for index, (name, directory, parent, size) in enumerate(entries):
        offset = 0xb000 + index * 64
        encoded = name.encode()
        data[offset:offset+len(encoded)] = encoded
        data[offset+0x28] = len(encoded) | (0x80 if directory else 0)
        data[offset+0x2f] = 1
        struct.pack_into('>H', data, offset+0x32, parent)
        struct.pack_into('>I', data, offset+0x34, size)
    data[0xc000:0xc004] = b'test'
    return data


class ExtractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / 'extracted'

    def run_package(self, data, *, list_only=False, preexec_fn=None):
        source = self.root / 'package.stfs'
        source.write_bytes(data)
        return subprocess.run(COMMAND + [str(source), str(self.output)] +
                              (['--list'] if list_only else []), capture_output=True,
                              timeout=5, preexec_fn=preexec_fn)

    def test_valid_nested_and_empty(self):
        result = self.run_package(package([('nested', True, 0xffff, 0),
                                           ('payload.bin', False, 0, 4),
                                           ('empty', False, 0xffff, 0)]))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / 'nested/payload.bin').read_bytes(), b'test')
        self.assertEqual((self.output / 'empty').read_bytes(), b'')
        self.assertEqual((self.output / 'package_license.toml').read_text(),
                         "# Written from the package's license block when the game was installed.\nlicense_mask = 0x1\n")

    def test_invalid_names(self):
        for name in ('../escaped.bin', str(self.root / 'absolute'), 'a/b', 'a\\b', 'C:evil', '.', '..',
                     'embedded\0null', 'x' * 41):
            with self.subTest(name=name):
                result = self.run_package(package([(name, False, 0xffff, 4)]))
                self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / 'escaped.bin').exists())
        self.assertFalse(self.output.exists())

    def test_invalid_parent(self):
        for entries in ([('payload.bin', False, 9, 4)],
                        [('payload.bin', False, 0, 4)],
                        [('one', True, 1, 0), ('two', True, 0, 0)]):
            with self.subTest(entries=entries):
                self.assertNotEqual(self.run_package(package(entries), list_only=True).returncode, 0)

    def test_truncated_listing(self):
        self.assertNotEqual(self.run_package(package()[:0x9800], list_only=True).returncode, 0)

    def test_listing_chain(self):
        for next_block in (0, 0xffffff, 0xfffffe):
            with self.subTest(next_block=next_block):
                self.assertNotEqual(self.run_package(package(table_count=2, table_next=next_block),
                                                    list_only=True).returncode, 0)

    def test_data_chain(self):
        for next_block in (1, 0xffffff, 0xfffffe):
            with self.subTest(next_block=next_block):
                data = package([('payload.bin', False, 0xffff, 8192)], data_next=next_block)
                self.assertNotEqual(self.run_package(data).returncode, 0)
                self.assertFalse((self.output / 'payload.bin').exists())

    def test_bad_declared_bounds(self):
        for offset, value in ((0x340, 0xffffffff), (0x395, 0), (0xb034, 0xffffffff)):
            with self.subTest(offset=offset):
                data = package()
                struct.pack_into('>I', data, offset, value)
                self.assertNotEqual(self.run_package(data).returncode, 0)

    def test_symlinks(self):
        outside = self.root / 'outside'
        outside.mkdir()
        for name, directory in (('payload.bin', False), ('nested', True),
                                ('package_license.toml', False)):
            with self.subTest(name=name):
                self.output.mkdir(exist_ok=True)
                target = outside / name
                if directory:
                    target.mkdir()
                else:
                    target.write_bytes(b'unchanged')
                link = self.output / name
                link.symlink_to(target, target_is_directory=directory)
                data = package([('nested', True, 0xffff, 0), ('payload.bin', False, 0, 4)]) if directory else package()
                self.assertNotEqual(self.run_package(data).returncode, 0)
                link.unlink()
                if not directory:
                    self.assertEqual(target.read_bytes(), b'unchanged')
        self.assertFalse((outside / 'nested/payload.bin').exists())

    def test_unwritable_directory(self):
        self.output.write_bytes(b'not a directory')
        self.assertNotEqual(self.run_package(package()).returncode, 0)

    def test_disk_full_preserves_existing(self):
        def limit_writes():
            resource.setrlimit(resource.RLIMIT_FSIZE, (2, 2))
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        self.output.mkdir()
        target = self.output / 'payload.bin'
        target.write_bytes(b'original')
        self.assertNotEqual(self.run_package(package(), preexec_fn=limit_writes).returncode, 0)
        self.assertEqual(target.read_bytes(), b'original')
        self.assertEqual(sorted(p.name for p in self.output.iterdir()), ['payload.bin'])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])

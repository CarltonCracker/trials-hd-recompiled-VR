#!/usr/bin/env python3
"""Exercise the desktop build script without an SDK, compiler, or game assets."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

MOCK_TOOL = r'''
import json, os
from pathlib import Path
import sys
name = Path(sys.argv[0]).name
args = sys.argv[1:]
root = Path(os.environ['MOCK_ROOT'])
if name == 'uname':
    print(os.environ['MOCK_OS'] if args == ['-s'] else os.environ['MOCK_ARCH'])
elif name in ('sysctl', 'nproc'):
    print(4)
elif name == 'cmake':
    with (root / 'calls.jsonl').open('a') as log:
        log.write(json.dumps(args) + '\n')
    if '--build' in args:
        build = Path(args[1])
        target = args[args.index('--target') + 1]
        if target == 'trials_hd_codegen':
            if os.environ.get('MOCK_FAIL_CODEGEN'):
                sys.exit(1)
            (root / 'sources_ready').touch()
        elif target == 'trials_hd':
            assert (build / 'sources_configured').exists(), 'link without generated objects'
            (build / 'trials_hd').touch()
    else:
        build = (Path(args[args.index('-B') + 1]) if '-B' in args else
                 root / 'out/build' / args[args.index('--preset') + 1])
        build.mkdir(parents=True, exist_ok=True)
        (build / 'build.ninja').touch()
        if (root / 'sources_ready').exists():
            (build / 'sources_configured').touch()
        else:
            (build / 'trials_hd_codegen_ready.stamp').touch()
'''


class BuildScriptTest(unittest.TestCase):
    def run_build(self, system='Darwin', arch='arm64', configured=False,
                  ready=False, stale_stamp=False, extra=(), fail_codegen=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shutil.copy(ROOT / 'build.sh', root)
            for name in ('default.xex', 'data/data.pak', 'data/data_menu.pak',
                         'package_license.toml'):
                path = root / 'assets' / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('stub')
            generated = root / 'generated/default'
            generated.mkdir(parents=True)
            (generated / 'trials_hd_pch.h').touch()
            sdk = root / 'thirdparty/rexglue-sdk'
            sdk.mkdir(parents=True)
            (sdk / 'CMakeLists.txt').touch()
            tools = root / 'bin'
            tools.mkdir()
            mock = tools / 'mock'
            mock.write_text(f'#!{sys.executable}\n' + MOCK_TOOL)
            mock.chmod(0o755)
            for name in ('uname', 'sysctl', 'nproc', 'cmake', 'ninja', 'clang++', 'emcmake'):
                (tools / name).symlink_to(mock)
            platform = 'mac' if system == 'Darwin' else 'linux'
            preset = ('web-wasm32-release' if '--web' in extra else
                      f'{platform}-{"arm64" if arch == "arm64" else "amd64"}-relwithdebinfo')
            build = root / 'out/build' / preset
            if ready:
                (root / 'sources_ready').touch()
            if configured:
                build.mkdir(parents=True)
                (build / 'build.ninja').touch()
                if ready:
                    (build / 'sources_configured').touch()
            if stale_stamp:
                (build / 'trials_hd_codegen_ready.stamp').touch()
            env = dict(os.environ, PATH=f'{tools}{os.pathsep}{os.environ["PATH"]}',
                       MOCK_ROOT=str(root), MOCK_OS=system, MOCK_ARCH=arch)
            if fail_codegen:
                env['MOCK_FAIL_CODEGEN'] = '1'
            # /bin/bash is macOS's stock Bash 3.2, including its empty-array behavior.
            result = subprocess.run(['/bin/bash', str(root / 'build.sh'), '-j4', *extra],
                                    env=env, capture_output=True, text=True)
            log = root / 'calls.jsonl'
            calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            calls = [[arg.replace(str(root), '<root>') for arg in call] for call in calls]
            return result, calls, (build / 'trials_hd_codegen_ready.stamp').exists(), preset

    def test_first_build(self):
        for system, arch in (('Darwin', 'arm64'), ('Darwin', 'x86_64'), ('Linux', 'x86_64')):
            with self.subTest(system=system, arch=arch):
                result, calls, stamp, preset = self.run_build(system=system, arch=arch)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                build = f'<root>/out/build/{preset}'
                self.assertEqual(calls, [
                    ['-S', '<root>', '--preset', preset],
                    ['--build', build, '-j4', '--target', 'trials_hd_codegen'],
                    ['-S', '<root>', '-B', build],
                    ['--build', build, '-j4', '--target', 'trials_hd'],
                ])
                self.assertFalse(stamp)

    def test_interrupted_first_build(self):
        for system, arch in (('Darwin', 'arm64'), ('Linux', 'x86_64')):
            with self.subTest(system=system):
                result, calls, stamp, _ = self.run_build(
                    system=system, arch=arch, configured=True, stale_stamp=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(calls), 3)
                self.assertFalse(stamp)

    def test_macos_flags_survive_reconfigure(self):
        result, calls, _, _ = self.run_build(extra=('--no-webgpu', '-DREXGLUE_ENABLE_TRACY=ON'))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls[0][-2:], ['-DTRIALSHD_WEBGPU=OFF', '-DREXGLUE_ENABLE_TRACY=ON'])
        self.assertIn('-B', calls[2])  # Reuse the cache rather than reset it via the preset.

    def test_codegen_failure_stops_build(self):
        result, calls, stamp, _ = self.run_build(fail_codegen=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(calls), 2)
        self.assertTrue(stamp)

    def test_custom_target_does_not_trigger_handoff(self):
        result, calls, _, _ = self.run_build(extra=('--target', 'webgpu_smoke'))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[-1][-2:], ['--target', 'webgpu_smoke'])

    def test_macos_web_build_does_not_trigger_handoff(self):
        result, calls, stamp, preset = self.run_build(
            configured=True, ready=True, stale_stamp=True, extra=('--web',))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls, [
            ['--build', f'<root>/out/build/{preset}', '-j4', '--target', 'trials_hd'],
        ])
        self.assertTrue(stamp)

    def test_linux_configure_with_flags(self):
        result, calls, _, preset = self.run_build(
            system='Linux', arch='x86_64', ready=True, extra=('-DTEST=ON',))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls, [
            ['-S', '<root>', '--preset', preset, '-DTEST=ON'],
            ['--build', f'<root>/out/build/{preset}', '-j4', '--target', 'trials_hd'],
        ])

    def test_incremental_build(self):
        for system, arch in (('Darwin', 'arm64'), ('Linux', 'x86_64')):
            with self.subTest(system=system):
                result, calls, _, preset = self.run_build(
                    system=system, arch=arch, configured=True, ready=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(calls, [
                    ['--build', f'<root>/out/build/{preset}', '-j4', '--target', 'trials_hd']])


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Tracy is off by default in optimized builds and can still be turned on.

Configures stub projects (no compilers, no SDK build) made of the Tracy block
from the top-level CMakeLists.txt followed by the SDK's own REXGLUE_ENABLE_TRACY
option line, then reads the value the SDK scope sees. Also configures the stub
through the real CMakePresets.json.

Usage: tracy_default_test.py [cmake executable]
"""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cmake = sys.argv[1] if len(sys.argv) > 1 else 'cmake'
ninja = shutil.which('ninja')

lines = (root / 'CMakeLists.txt').read_text().splitlines()
start = next(i for i, line in enumerate(lines) if line.startswith('# Tracy profiler:'))
end = lines.index('endif()', start)
block = '\n'.join(lines[start:end + 1])

sdk_option = next(line for line in (root / 'thirdparty/rexglue-sdk/CMakeLists.txt').read_text().splitlines()
                  if line.startswith('option(REXGLUE_ENABLE_TRACY '))
assert sdk_option.rstrip().endswith('ON)'), f'SDK default changed: {sdk_option}'

failures = []


def configure(stub, args):
    result = subprocess.run([cmake, *args], cwd=stub, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stdout + result.stderr)
    found = re.search(r'SDK_TRACY=(\S*)', result.stdout)
    assert found, result.stdout
    return found.group(1)


def check(name, stub, args, expected):
    got = configure(stub, args)
    status = 'ok' if got == expected else 'FAIL'
    print(f'{status:4} {name}: REXGLUE_ENABLE_TRACY={got} (want {expected})')
    if got != expected:
        failures.append(name)


with tempfile.TemporaryDirectory() as directory:
    stub = Path(directory) / 'stub'
    (stub / 'sdk').mkdir(parents=True)
    (stub / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.25)\nproject(stub LANGUAGES NONE)\n'
        f'{block}\nadd_subdirectory(sdk)\n')
    (stub / 'sdk/CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.25)\nproject(sdk LANGUAGES NONE)\n'
        f'{sdk_option}\nmessage(STATUS "SDK_TRACY=${{REXGLUE_ENABLE_TRACY}}")\n')
    shutil.copy(root / 'CMakePresets.json', stub)
    generator = ['-G', 'Ninja'] if ninja else []
    count = 0

    def single(name, build_type, extra, expected):
        global count
        count += 1
        build = stub / f'b{count}'
        check(name, stub,
              ['-S', '.', '-B', str(build), *generator, f'-DCMAKE_BUILD_TYPE={build_type}', *extra],
              expected)
        return build

    single('Release default', 'Release', [], 'OFF')
    single('RelWithDebInfo default', 'RelWithDebInfo', [], 'OFF')
    single('MinSizeRel default', 'MinSizeRel', [], 'OFF')
    single('Debug default', 'Debug', [], 'ON')
    single('no build type default', '', [], 'ON')
    single('Release explicit ON', 'Release', ['-DREXGLUE_ENABLE_TRACY=ON'], 'ON')
    build = single('RelWithDebInfo explicit ON', 'RelWithDebInfo', ['-DREXGLUE_ENABLE_TRACY=ON'], 'ON')
    check('RelWithDebInfo explicit ON survives reconfigure', stub, ['-S', '.', '-B', str(build)], 'ON')
    single('Debug explicit OFF', 'Debug', ['-DREXGLUE_ENABLE_TRACY=OFF'], 'OFF')

    if ninja:
        check('Ninja Multi-Config keeps SDK default', stub,
              ['-S', '.', '-B', str(stub / 'mc1'), '-G', 'Ninja Multi-Config'], 'ON')
        check('Ninja Multi-Config explicit OFF', stub,
              ['-S', '.', '-B', str(stub / 'mc2'), '-G', 'Ninja Multi-Config', '-DREXGLUE_ENABLE_TRACY=OFF'],
              'OFF')

        presets = {p['name']: p for p in json.loads((root / 'CMakePresets.json').read_text())['configurePresets']}
        host = 'Linux' if sys.platform.startswith('linux') else 'Darwin' if sys.platform == 'darwin' else None
        prefix = {'Linux': 'linux-', 'Darwin': 'mac-'}.get(host)
        if prefix:
            for name, preset in presets.items():
                if not name.startswith(prefix) or preset.get('hidden'):
                    continue
                expected = 'ON' if name.endswith('-debug') else 'OFF'
                check(f'preset {name}', stub, ['--preset', name], expected)
            rel = f'{prefix}amd64-relwithdebinfo' if host == 'Linux' else f'{prefix}arm64-relwithdebinfo'
            check(f'preset {rel} explicit ON', stub, ['--preset', rel, '-DREXGLUE_ENABLE_TRACY=ON'], 'ON')
            # A build dir configured before this default had ON cached; the preset resets it.
            check(f'preset {rel} overrides stale cached ON', stub, ['--preset', rel], 'OFF')
    else:
        print('skip multi-config and preset checks: ninja not found')

# The link probe follows actual compile flags, not a possibly stale Tracy .so.
if bash := shutil.which('bash'):
    probe = (root / 'docs/webgpu-renderer/probe/link_stub.sh').read_text()
    flags = probe[probe.index('tracy=()'):probe.index('clang++ -fPIC -pthread -shared')]
    for compile_flags, expected in [('', '0'), ('-DTRACY_ENABLE -DTRACY_IMPORTS', '1')]:
        result = subprocess.check_output(
            [bash, '-c', 'compile=$1; sdk_out=/unused; ' + flags + '\nprintf "%s" "${#tracy[@]}"',
             'probe', compile_flags], text=True)
        assert result == expected, (compile_flags, result)
    print('ok   link probe omits Tracy when disabled and retains it when enabled')

if failures:
    print(f'FAIL: {len(failures)} case(s): {", ".join(failures)}')
    sys.exit(1)
print('PASS: Tracy defaults off for optimized builds, on for Debug, explicit values honored')

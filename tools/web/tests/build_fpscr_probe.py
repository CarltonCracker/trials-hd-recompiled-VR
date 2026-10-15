#!/usr/bin/env python3
"""Relink a private native FPSCR probe; never edit generated code or native objects.

Requires the existing Linux Ninja build. Recompiles only the five translation units
containing storeFromGuest with a forced trace header. Output: out/web-spikes/fpscr-probe/.
"""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[3]
    build = root / 'out/build/linux-amd64-relwithdebinfo'
    output = root / 'out/web-spikes/fpscr-probe'
    output.mkdir(parents=True, exist_ok=True)
    sources = {p for p in (root / 'generated/default').glob('trials_hd_recomp.*.cpp')
               if 'storeFromGuest(' in p.read_text()}
    assert len(sources) == 5, f'Expected five rounding-mode sites, got {len(sources)} files'
    commands = subprocess.check_output(
        ['cmake', '--build', str(build), '--', '-t', 'commands', 'trials_hd'], text=True).splitlines()
    jobs = []
    replacements = {}
    link = None
    for command in commands:
        args = shlex.split(command)
        if '-c' in args and Path(args[args.index('-c') + 1]) in sources:
            destination = output / (Path(args[args.index('-c') + 1]).name + '.o')
            replacements[args[args.index('-o') + 1]] = str(destination)
            cleaned = []
            i = 0
            while i < len(args):
                if args[i] == '-Xclang' and args[i + 1] in ('-include-pch', '-include'):
                    assert args[i + 2] == '-Xclang'
                    i += 4
                elif args[i] in ('-o', '-MF', '-MT'):
                    i += 2
                elif args[i] in ('-MD', '-MP'):
                    i += 1
                else:
                    cleaned.append(args[i])
                    i += 1
            jobs.append(cleaned + ['-include', str(Path(__file__).with_name('native_fpscr_trace.h')),
                                  '-include', str(root / 'generated/default/trials_hd_pch.h'),
                                  '-o', str(destination)])
        if '-o' in args and args[args.index('-o') + 1] == 'trials_hd':
            assert args[:2] == [':', '&&']
            end = args.index('&&', 2) if '&&' in args[2:] else len(args)
            link = args[2:end]
    assert len(jobs) == len(sources) and link is not None, 'Native Ninja commands not found'
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda args: subprocess.run(args, cwd=build, check=True), jobs))
    result = []
    i = 0
    while i < len(link):
        if link[i] == '-Xlinker' and link[i + 1].startswith('--dependency-file='):
            i += 2  # Do not overwrite the native link dependency file.
            continue
        arg = replacements.get(link[i], link[i])
        if i and link[i - 1] == '-o':
            arg = str(output / 'trials_hd')
        if arg.startswith('-Wl,-rpath,'):
            arg = '-Wl,-rpath,$ORIGIN'
        result.append(arg)
        i += 1
    assert all(str(p) in result for p in (output.glob('*.cpp.o')))
    subprocess.run(result, cwd=build, check=True)
    for library in build.glob('*.so'):
        destination = output / library.name
        if not destination.exists():
            destination.symlink_to(library)
    print(f'Native FPSCR probe built: {output / "trials_hd"}; original build unchanged')


if __name__ == '__main__':
    main()

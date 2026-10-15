#!/usr/bin/env python3
"""Compile the user's generated game code for wasm64, without regenerating it.

Activate emsdk first. Objects are private build artifacts under out/, never publish.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-j', '--jobs', type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    compiler = shutil.which('em++')
    if not compiler:
        parser.error('activate Emscripten first: source ~/emsdk/emsdk_env.sh')
    root = Path(__file__).resolve().parents[2]
    sdk = root / 'thirdparty/rexglue-sdk'
    generated = root / 'generated/default'
    sources = sorted(generated.glob('trials_hd_recomp.[0-9]*.cpp'))
    if not sources or not (generated / 'trials_hd_pch.h').is_file():
        parser.error('run the native build/codegen first; generated/default is missing')
    output = root / 'out/web-spikes/recomp-objects'
    output.mkdir(parents=True, exist_ok=True)
    includes = [sdk / 'include', sdk / 'thirdparty/simde', sdk / 'thirdparty/fmt/include',
                sdk / 'thirdparty/spdlog/include', sdk / 'thirdparty/tomlplusplus/include']
    flags = ['--target=wasm64', '-O2', '-std=gnu++23', '-msimd128', '-pthread',
             '-matomics', '-mbulk-memory', '-fwasm-exceptions',
             '-DSPDLOG_COMPILED_LIB', '-DSPDLOG_FMT_EXTERNAL',
             *('-I' + str(path) for path in includes),
             '-include', str(generated / 'trials_hd_pch.h')]

    def compile_source(source):
        destination = output / (source.stem + '.o')
        subprocess.run([compiler, *flags, '-c', str(source), '-o', str(destination)], check=True)
        assert destination.stat().st_size > 0
        return source.name

    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for count, name in enumerate(pool.map(compile_source, sources), 1):
            print(f'{count}/{len(sources)} {name}', flush=True)
    print(f'Wasm64 generated-code compile PASSED: {len(sources)} files, '
          f'{time.monotonic() - start:.1f}s, no scratch headers or regeneration')


if __name__ == '__main__':
    main()

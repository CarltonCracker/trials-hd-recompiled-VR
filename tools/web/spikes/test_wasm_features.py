#!/usr/bin/env python3
"""Reassemble the feature probes to catch malformed or mismatched byte fixtures.

Usage: python3 tools/web/spikes/test_wasm_features.py ~/emsdk/upstream/bin/wasm-as
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    page = Path(__file__).with_name('wasm_features.html').read_text()
    fixtures = re.findall(r"(\w+): \{\s*wat: '([^']+)',\s*hex: '([0-9a-f]+)'", page)
    assert len(fixtures) == 5, 'Expected all five binary feature fixtures'
    assembler = sys.argv[1] if len(sys.argv) > 1 else 'wasm-as'
    with tempfile.TemporaryDirectory(prefix='trials-wasm-fixtures-') as temporary:
        root = Path(temporary)
        for name, wat, hex_bytes in fixtures:
            source, output = root / (name + '.wat'), root / (name + '.wasm')
            source.write_text(wat + '\n')
            subprocess.run([assembler, str(source), '-o', str(output), '--all-features'], check=True)
            assert output.read_bytes() == bytes.fromhex(hex_bytes), f'{name}: binary fixture differs'
            print(f'{name}: valid WAT, identical {len(output.read_bytes())}-byte fixture')
    print('W-S02 fixture checks PASSED')


if __name__ == '__main__':
    main()

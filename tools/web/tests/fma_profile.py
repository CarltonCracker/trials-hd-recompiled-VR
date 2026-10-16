#!/usr/bin/env python3
"""Summarize a finished Node FMA run (profiles/, game.log and optional node.log).

Usage: python3 tools/web/tests/fma_profile.py out/fma-before > out/fma-before/summary.json
Reports the last part of the run (10 s warmup excluded), excluding futex waits
from active wasm samples. Raw profiles/game logs stay private under out/.
"""

from collections import Counter
import json
from pathlib import Path
import sys


def summarize(data, warmup_us=10000000):
    nodes = {node['id']: node for node in data['nodes']}
    hot = {ident for ident, node in nodes.items() if node['callFrame']['functionName'] == 'fma'}
    pending = list(hot)
    while pending:
        for child in nodes[pending.pop()].get('children', []):
            if child not in hot:
                hot.add(child)
                pending.append(child)
    elapsed = 0
    counts = Counter()
    for ident, delta in zip(data.get('samples', []), data.get('timeDeltas', []), strict=True):
        elapsed += delta
        if elapsed >= warmup_us:
            counts[ident] += 1
    active = sum(count for ident, count in counts.items()
                 if nodes[ident]['callFrame'].get('url', '').startswith('wasm://')
                 and nodes[ident]['callFrame']['functionName'] != '_do_futex_wait')
    fma = sum(count for ident, count in counts.items() if ident in hot)
    total = sum(counts.values())
    return {'samples': total, 'activeWasmSamples': active, 'fmaSamples': fma,
            'activeFmaPercent': 100 * fma / active if active else 0,
            'allSampleFmaPercent': 100 * fma / total if total else 0}


def self_check():
    data = {'nodes': [
        {'id': 1, 'callFrame': {'functionName': 'fma', 'url': 'wasm://test'}, 'children': [2]},
        {'id': 2, 'callFrame': {'functionName': 'normalize', 'url': 'wasm://test'}},
        {'id': 3, 'callFrame': {'functionName': 'work', 'url': 'wasm://test'}},
        {'id': 4, 'callFrame': {'functionName': '_do_futex_wait', 'url': 'wasm://test'}},
    ], 'samples': [1, 2, 3, 4, 3], 'timeDeltas': [1, 1, 1, 1, 1]}
    result = summarize(data, 0)
    assert (result['samples'], result['activeWasmSamples'], result['fmaSamples']) == (5, 4, 2)
    assert result['activeFmaPercent'] == 50 and result['allSampleFmaPercent'] == 40
    assert summarize(data, 3)['fmaSamples'] == 0


def main():
    self_check()
    run = Path(sys.argv[1])
    logs = ''.join(path.read_text() for path in (run / 'game.log', run / 'node.log') if path.exists())
    assert 'first guest swap' in logs, 'Game boot not verified in this run'
    paths = sorted((run / 'profiles').glob('*.cpuprofile'))
    assert paths, 'No CPU profiles (stop Node cleanly before summarizing)'
    workers = [{'profile': path.name, **summarize(json.loads(path.read_text()))} for path in paths]
    active = sum(worker['activeWasmSamples'] for worker in workers)
    fma = sum(worker['fmaSamples'] for worker in workers)
    assert active, 'No active wasm samples after warmup'
    # Tiny workers have noisy percentages; retain every row and report busy workers separately.
    busy = [worker for worker in workers if worker['activeWasmSamples'] >= 1000]
    print(json.dumps({'warmupSeconds': 10, 'activeWasmSamples': active, 'fmaSamples': fma,
                      'activeFmaPercent': 100 * fma / active,
                      'maxBusyWorkerFmaPercent': max((w['activeFmaPercent'] for w in busy), default=0),
                      'workers': workers}, indent=2))


if __name__ == '__main__':
    main()

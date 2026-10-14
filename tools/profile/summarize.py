#!/usr/bin/env python3
"""Summarize gdb_sampler.py output per thread: share of samples busy, spinning
(sched_yield and friends), in the GPU driver (ioctl) or idle (blocked in a
wait), and the hottest functions of the busy samples.

Usage: summarize.py <samples.txt> [--top N] [--thread SUBSTRING]
"""
import collections
import re
import sys

WAIT = re.compile(r'Wait|wait|Sleep|sleep|nanosleep|epoll|poll|select|futex|sigtimedwait|'
                  r'accept|recv|^read$|__libc_read|pthread_cond|KeDelayExecution')
SPIN = re.compile(r'sched_yield|yield_processor|spin_once|MaybeYield')
DRIVER = re.compile(r'ioctl|drmIoctl')


def classify(stack):
    # Look past unsymbolized libc frames (syscall wrappers) to the caller.
    for name in stack[:10]:
        if SPIN.search(name):
            return 'spin'
        if DRIVER.search(name):
            return 'driver'
        if WAIT.search(name):
            return 'idle'
        if '@libc' in name or name.startswith('0x'):
            continue
        return 'busy'
    return 'busy'


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    top, only = 15, None
    if '--top' in args:
        i = args.index('--top'); top = int(args[i + 1]); del args[i:i + 2]
    if '--thread' in args:
        i = args.index('--thread'); only = args[i + 1]; del args[i:i + 2]
    threads = collections.defaultdict(list)
    for line in open(args[0], errors='replace'):
        name, _, stack = line.rstrip('\n').partition('\t')
        threads[name].append(stack.split(';') if stack else [])
    rows = []
    for name, stacks in threads.items():
        kinds = collections.Counter(classify(s) for s in stacks)
        rows.append((1 - kinds['idle'] / len(stacks), name, stacks, kinds))
    rows.sort(key=lambda r: -r[0])
    for active, name, stacks, kinds in rows:
        if only and only not in name:
            continue
        if not only and active < 0.05:
            continue
        n = len(stacks)
        print(f"\n=== {name}: {n} samples  busy {kinds['busy'] / n:.0%}  spin {kinds['spin'] / n:.0%}"
              f"  driver {kinds['driver'] / n:.0%}  idle {kinds['idle'] / n:.0%}")
        busy = [s for s in stacks if classify(s) == 'busy']
        self_counts = collections.Counter(s[0] for s in busy if s)
        inclusive = collections.Counter()
        for s in busy:
            inclusive.update(set(f for f in s if not f.startswith('std::')))
        print('  self:')
        for fn, c in self_counts.most_common(top):
            print(f'    {c / n:6.1%}  {fn[:110]}')
        print('  inclusive:')
        for fn, c in inclusive.most_common(top):
            print(f'    {c / n:6.1%}  {fn[:110]}')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Per-thread CPU use of a running process over an interval (from /proc).

Usage: thread_cpu.py <pid or process name> [seconds=10] [--top N]
CPU% is of one core. Spin-waiting threads count as busy here; use the sampler
to tell spinning from work.
"""
import os
import sys
import time


def find_pid(arg):
    if arg.isdigit():
        return arg
    for p in os.listdir('/proc'):
        if p.isdigit():
            try:
                if open(f'/proc/{p}/comm').read().strip() == arg:
                    return p
            except OSError:
                pass
    raise SystemExit(f'no process named {arg}')


def snapshot(pid):
    result = {}
    for tid in os.listdir(f'/proc/{pid}/task'):
        try:
            stat = open(f'/proc/{pid}/task/{tid}/stat').read()
        except OSError:
            continue
        name = stat[stat.index('(') + 1:stat.rindex(')')]
        fields = stat[stat.rindex(')') + 2:].split()
        result[tid] = (name, int(fields[11]) + int(fields[12]))  # utime + stime
    return result


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    top = 15
    if '--top' in args:
        i = args.index('--top'); top = int(args[i + 1]); del args[i:i + 2]
    pid = find_pid(args[0])
    seconds = float(args[1]) if len(args) > 1 else 10.0
    hz = os.sysconf('SC_CLK_TCK')
    before = snapshot(pid)
    time.sleep(seconds)
    after = snapshot(pid)
    rows = sorted(((ticks - before[tid][1]) / hz / seconds * 100, name, tid)
                  for tid, (name, ticks) in after.items() if tid in before)[::-1]
    print(f'process total {sum(r[0] for r in rows):.0f}% of one core over {seconds:g}s')
    for cpu, name, tid in rows[:top]:
        print(f'{cpu:6.1f}%  {name:16s} tid {tid}')


if __name__ == '__main__':
    main()

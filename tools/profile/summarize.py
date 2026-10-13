#!/usr/bin/env python3
"""Summarize gdb_sampler.py output: per thread, the share of samples that were
running (not blocked in a wait), and the hottest functions (self and inclusive).

Usage: summarize.py <sampler.txt> [--top N] [--thread SUBSTRING]
"""
import collections
import signal
import sys

# Leaf frames that mean the thread was blocked, not running.
IDLE = ("futex", "sleep_for", "Sleep", "WaitMultiple", "Wait", "nanosleep", "clock_nanosleep", "poll", "epoll_wait", "select",
        "pthread_cond_wait", "pthread_cond_timedwait", "__GI___pthread_cond",
        "sem_wait", "sem_timedwait", "read", "__libc_read", "recv", "accept",
        "sigtimedwait", "sigwait", "pause", "wait4", "__lll_lock_wait", "ppoll",
        "__syscall_cancel_arch", "syscall")


def is_idle(frames):
    # Look past unsymbolized libc frames (syscall wrappers) to the wait call.
    for name in frames[:4]:
        if any(name.startswith(p) or name.endswith(p) for p in IDLE):
            return True
        if "@libc" not in name:
            break
    return False


def short(name, width=90):
    return name if len(name) <= width else name[:width - 3] + "..."


def main():
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    top = 12
    only = None
    if "--top" in args:
        i = args.index("--top"); top = int(args[i + 1]); del args[i:i + 2]
    if "--thread" in args:
        i = args.index("--thread"); only = args[i + 1]; del args[i:i + 2]
    threads = collections.defaultdict(list)
    with open(args[0], errors="replace") as f:
        for line in f:
            name, _, stack = line.rstrip("\n").partition("\t")
            threads[name].append(stack.split(";") if stack else [])
    rows = []
    for name, stacks in threads.items():
        busy = [s for s in stacks if not is_idle(s)]
        rows.append((len(busy) / len(stacks), name, stacks, busy))
    rows.sort(key=lambda r: -r[0])
    for share, name, stacks, busy in rows:
        if only and only not in name:
            continue
        if share < 0.02 and not only:
            continue
        print(f"\n=== {name}: running in {share * 100:.0f}% of {len(stacks)} samples")
        self_counts = collections.Counter(s[0] for s in busy if s)
        incl = collections.Counter()
        for s in busy:
            for fn in set(s):
                incl[fn] += 1
        print("  self:")
        for fn, n in self_counts.most_common(top):
            print(f"    {n / len(stacks) * 100:5.1f}%  {short(fn)}")
        print("  inclusive:")
        for fn, n in incl.most_common(top):
            print(f"    {n / len(stacks) * 100:5.1f}%  {short(fn)}")


if __name__ == "__main__":
    main()

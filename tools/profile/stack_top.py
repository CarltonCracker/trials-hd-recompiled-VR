#!/usr/bin/env python3
"""Summarize eu-stack samples (tools/profile/thread_sample.sh) per thread.

Usage: stack_top.py <stacks.txt> <tid> [--top N] [--inclusive]
Prints the most frequent leaf functions (or, with --inclusive, functions
anywhere in the stack) of one thread as a share of its samples.
"""
import collections
import re
import sys

FRAME = re.compile(r"^#\d+\s+0x[0-9a-f]+\s+(.*?)\s*(?:-\s.*)?$")


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    path, tid = args[0], args[1]
    top = int(args[args.index("--top") + 1]) if "--top" in args else 25
    inclusive = "--inclusive" in args
    counts = collections.Counter()
    samples = 0
    stack = None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip()
            if line.startswith("TID "):
                if stack is not None:
                    samples += 1
                    names = stack if inclusive else stack[:1]
                    for name in set(names) if inclusive else names:
                        counts[name] += 1
                stack = [] if line[4:].rstrip(":") == tid else None
                continue
            if stack is not None:
                m = FRAME.match(line)
                if m:
                    stack.append(m.group(1) or "?")
    if stack is not None:
        samples += 1
        for name in set(stack) if inclusive else stack[:1]:
            counts[name] += 1
    print(f"{samples} samples of thread {tid}")
    for name, n in counts.most_common(top):
        print(f"{100 * n / max(samples, 1):6.1f}%  {name}")


if __name__ == "__main__":
    main()

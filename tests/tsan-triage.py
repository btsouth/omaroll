#!/usr/bin/env python3
"""Sort ThreadSanitizer reports into ours and external, and fail on ours.

Arch's Qt and glibc are not built with TSan, so TSan cannot see the locks and
atomics inside them. Qt allocates an event on one thread and frees it on
another under its own mutex, and TSan reports that as a race. Such a report is
external when every access in it starts inside an uninstrumented library
(after skipping allocator and pthread interceptors). Any report where an
access starts in an instrumented binary (Omaroll's code, or a Qt header
compiled into it) fails the job.

Usage: tsan-triage.py <report files...>
"""
import re
import sys
from pathlib import Path

INSTRUMENTED = re.compile(r"\((omaroll[^+]*)\+0x[0-9a-f]+\)")
MODULE = re.compile(r"\(([^()+]+)\+0x[0-9a-f]+\)\s*(\(BuildId: [0-9a-f]+\))?\s*$")
ACCESS = re.compile(r"^  (Previous )?(atomic )?(Read|Write|read|write) of size|^  .* by (main thread|thread T\d+)")
INTERCEPTOR = re.compile(
    r"^\s*#\d+ (operator new|operator delete|malloc|calloc|realloc|free|strdup|"
    r"memcpy|memmove|memset|pthread_\w+|__tsan\w*|QtTsan::\w+)\b")


def first_real_module(frames):
    for frame in frames:
        if INTERCEPTOR.match(frame):
            continue
        match = MODULE.search(frame)
        return match.group(1) if match else None
    return None


def classify(block):
    lines = block.splitlines()
    stacks, current = [], None
    for line in lines:
        if ACCESS.match(line) and "created by" not in line and "allocated by" not in line:
            current = []
            stacks.append(current)
        elif current is not None and re.match(r"^\s*#\d+ ", line):
            current.append(line)
        elif current is not None and not line.strip():
            current = None
    if not stacks:
        return "ours"  # Not a two-access race (double lock, signal, leak): look at it.
    for frames in stacks:
        module = first_real_module(frames)
        if module is None or module.startswith("omaroll"):
            return "ours"
    return "external"


def main(paths):
    ours, external = [], []
    for path in paths:
        text = Path(path).read_text(errors="replace")
        for block in text.split("==================")[1:]:
            if "WARNING: ThreadSanitizer" not in block:
                continue
            (ours if classify(block) == "ours" else external).append(block.strip())
    for block in external:
        title = block.splitlines()[0]
        print(f"external (uninstrumented library internals): {title}")
    for block in ours:
        print("==================\n" + block)
    print(f"tsan-triage: {len(ours)} report(s) in instrumented code, {len(external)} external")
    return 1 if ours else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

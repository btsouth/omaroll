#!/usr/bin/env python3
"""Sort ThreadSanitizer reports into ours and external, and fail on ours.

Arch's Qt and glibc are not built with TSan, so TSan cannot see the locks and
atomics inside them, and a few of Qt's own handoffs show up as races. Every
report counts as ours unless each of its access stacks starts in one of the
Qt internals listed in KNOWN_QT_HANDOFFS. Where a stack starts is its first
frame after TSan's own interceptors (malloc, memcpy, pthread_*).

Matching our source files is not enough. Qt is built without frame pointers,
so the frame that called into Qt is often missing from the stack: a race on a
QString that two of our threads append to can show only Qt and libstdc++
frames. Reports therefore fail closed.

A TSan CHECK failure ends the process, so it fails the run as well.

Usage: tsan-triage.py <report files...>
       tsan-triage.py --self-test
"""
import re
import sys
from pathlib import Path

# Qt internals whose races are Qt's own synchronization, invisible to TSan.
# Each entry needs a report that shows it, and the reason it is not ours.
KNOWN_QT_HANDOFFS = [
    # A QFutureInterface's wait condition is destroyed after the worker that
    # waited on it returned, ordered by Qt's mutex (seen in MimeDefaults).
    r"QWaitCondition::(~QWaitCondition|wait)\(",
]
HANDOFF = re.compile("|".join(KNOWN_QT_HANDOFFS))
# A QtConcurrent::run task: the functor and its captures are copied into the
# task on the submitting thread, then QThreadPool::start hands the task over
# under QThreadPool's mutex, which TSan cannot see. A pool-side access while
# StoredFunctionCall copies its stored arguments out (or the pool calls the
# task's run, reading its vtable), against the task being
# built (or the captured data being allocated) on the submitting thread, is
# that handoff. Seen for DocumentTextIndex::processNext.
TASK_RUN = re.compile(r"QtConcurrent::(StoredFunctionCall<.*>::runFunctor|RunFunctionTaskBase<.*>::run)\(")
TASK_START = re.compile(r"QtConcurrent::(NonPromiseTaskResolver<|run<)")
ALLOCATION = re.compile(r"^(operator new|malloc|calloc|realloc)\b")
# QThreadPool's own queue, touched by two pooled threads in libQt6Core code
# with nothing of ours on either stack, in a block a pooled thread allocated
# there. Our code on a pooled thread always has frames of our own, since it
# is built with frame pointers.
POOL_THREAD = "Thread (pooled)"
# Qt threads whose end TSan never sees, since Qt joins its threads inside
# libQt6Core. QtTest's watchdog waits on a condition variable on the main
# thread's stack, and that stack is reused after the watchdog is gone.
KNOWN_QT_THREADS = ["QtTest Watchdog"]
FRAME = re.compile(r"^\s*#\d+ (.*?) (?:<null>|\S+:\d+(?::\d+)?) \(([^()+]+)\+0x[0-9a-f]+\)")
ACCESS = re.compile(r"^  (Previous )?([Aa]tomic )?(Read|Write|read|write) of size .* by (main thread|thread (T\d+))")
THREAD = re.compile(r"^  Thread (T\d+) '([^']*)'", re.M)
INTERCEPTOR = re.compile(
    r"^(operator new|operator delete|malloc|calloc|realloc|free|strdup|"
    r"memcpy|memmove|memset|pthread_\w+|__tsan\w*|QtTsan::\w+)\b")
FATAL = re.compile(r"ThreadSanitizer: CHECK failed|ThreadSanitizer: .*(out of memory|internal error)")


def first_frame(frames):
    """The first frame after TSan's interceptors, as (function, module)."""
    for frame in frames:
        match = FRAME.match(frame)
        if not match:
            continue
        function, module = match.groups()
        # The interceptors live in the instrumented binary. A libc frame that
        # happens to be called strdup or free is libc itself.
        if INTERCEPTOR.match(function) and module.startswith("omaroll"):
            continue
        return function, module
    return None, None


def access_stacks(block):
    """The frames of each access, and the threads that made them."""
    stacks, threads, current = [], [], None
    for line in block.splitlines():
        access = ACCESS.match(line)
        if access:
            current = []
            stacks.append(current)
            threads.append(access.group(5) or "main")
        elif current is not None and re.match(r"^\s*#\d+ ", line):
            current.append(line)
        elif current is not None and not line.strip():
            current = None
    return stacks, threads


def frame_functions(frames):
    out = []
    for frame in frames:
        match = FRAME.match(frame)
        if match:
            out.append(match.groups())
    return out


def task_start_handoff(stacks):
    if len(stacks) != 2:
        return False
    for task, other in (stacks, stacks[::-1]):
        if not any(TASK_RUN.search(frame) for frame in task):
            continue
        if any(TASK_START.search(frame) for frame in other):
            return True
        frames = frame_functions(other)
        if frames and ALLOCATION.match(frames[0][0]):
            return True
    return False


def only_qt_pool_frames(frames):
    """An interceptor in our binary, then unnamed libQt6Core frames only."""
    functions = frame_functions(frames)
    if len(functions) < 2 or not INTERCEPTOR.match(functions[0][0]):
        return False
    return all(function == "<null>" and module.startswith("libQt6Core")
               for function, module in functions[1:])


def pool_bookkeeping(block, stacks, threads):
    names = dict(THREAD.findall(block))
    if len(stacks) != 2 or any(names.get(thread) != POOL_THREAD for thread in threads):
        return False
    location = re.search(r"^  Location is heap block .* allocated by thread (T\d+):\n((?:\s*#\d+ .*\n?)+)",
                         block, re.M)
    if not location or names.get(location.group(1)) != POOL_THREAD:
        return False
    return all(only_qt_pool_frames(frames) for frames in stacks + [location.group(2).splitlines()])


def classify(block):
    if FATAL.search(block):
        return "fatal"
    stacks, threads = access_stacks(block)
    if not stacks:
        return "ours"  # Not a two-access race (double lock, signal, leak): look at it.
    names = dict(THREAD.findall(block))
    if any(names.get(thread) in KNOWN_QT_THREADS for thread in threads):
        return "external"
    if task_start_handoff(stacks) or pool_bookkeeping(block, stacks, threads):
        return "external"
    for frames in stacks:
        function, module = first_frame(frames)
        if function is None or module.startswith("omaroll") or not HANDOFF.search(function):
            return "ours"
    return "external"


def reports(text):
    # A CHECK failure is printed after the last separator, outside any block.
    for block in text.split("==================")[1:] if "==================" in text else [text]:
        if "WARNING: ThreadSanitizer" in block or FATAL.search(block):
            yield block.strip()


def triage(paths):
    ours, external = [], []
    for path in paths:
        for block in reports(Path(path).read_text(errors="replace")):
            (external if classify(block) == "external" else ours).append(block)
    for block in external:
        print(f"external (known Qt handoff): {block.splitlines()[0]}")
    for block in ours:
        print("==================\n" + block)
    print(f"tsan-triage: {len(ours)} report(s) to fix, {len(external)} known Qt handoff(s)")
    return 1 if ours else 0


def self_test():
    """Each fixture is named for the class its report must get."""
    failures = 0
    fixtures = sorted((Path(__file__).parent / "tsan-triage").glob("*.txt"))
    for fixture in fixtures:
        expected = fixture.name.split("-", 1)[0]
        got = [classify(block) for block in reports(fixture.read_text())]
        if not got or any(kind != expected for kind in got):
            print(f"tsan-triage self-test: {fixture.name}: expected {expected}, got {got}")
            failures += 1
    print(f"tsan-triage self-test: {len(fixtures) - failures} of {len(fixtures)} fixtures sorted correctly")
    return 1 if failures or not fixtures else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    sys.exit(triage(sys.argv[1:]))

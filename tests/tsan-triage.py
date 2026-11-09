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


# A lambda's capture block being moved or copied, "Class::method()::$_0::$_0(".
CAPTURE_COPY = re.compile(r"::\$_\d+::\$_\d+\(")
# A copy or move constructor, "X::X(X const&)" or "X::X(X&&)", the only code of
# ours that copying a capture runs.
COPY_CONSTRUCTOR = re.compile(r"(?:^|::)(\w+)(?:<.*>)?::\1\((?:[\w:]*::)?\1(?:<.*>)?(?: const&|&&)\)")
FRAME_SOURCE = re.compile(r"^\s*#\d+ .*? (/\S+):\d+")


def handing_over(task):
    """The pooled side is still taking the task over: reading its vtable
    (RunFunctionTaskBase::run is the first frame), or copying the captures out
    of the stored call, with only Qt and libstdc++ headers and copy
    constructors on the way. Our own lambda body running is not this."""
    functions = frame_functions(task)
    if functions and TASK_RUN.search(functions[0][0]) and "RunFunctionTaskBase" in functions[0][0]:
        return True
    for index, frame in enumerate(task):
        if not CAPTURE_COPY.search(frame):
            continue
        if not any("runFunctor" in later for later in task[index + 1:index + 6]):
            return False
        for earlier in task[:index]:
            source = FRAME_SOURCE.match(earlier)
            match = FRAME.match(earlier)
            header = source is not None and source.group(1).startswith("/usr/")
            copy = match is not None and COPY_CONSTRUCTOR.search(match.group(1))
            if not header and not copy:
                return False
        return True
    return False


def task_start_handoff(stacks):
    if len(stacks) != 2:
        return False
    for task, other in (stacks, stacks[::-1]):
        if not any(TASK_RUN.search(frame) for frame in task) or not handing_over(task):
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


def qt_only(frames):
    """No frame of ours past the interceptor. TSan builds race stacks from the
    calls it saw instrumented code make, so a stack like this means none of
    our code, header templates included, was running on that thread: Qt was
    at work on its own, as in QImage's threaded conversions or QThreadPool's
    queue."""
    functions = frame_functions(frames)
    return bool(functions) and all(not module.startswith("omaroll") or INTERCEPTOR.match(function)
                                   for function, module in functions)


def made_by_library(frames):
    """The access was made inside a library call (Qt, libpng), not by us."""
    functions = frame_functions(frames)
    for function, module in functions:
        if module.startswith("omaroll") and INTERCEPTOR.match(function):
            continue
        return not module.startswith("omaroll")
    return False


def allocated_as_qimage(block):
    """The memory is a QImage's pixel buffer, which Qt's own threads convert
    and encode (QImage's threaded conversions, the PNG writer)."""
    location = re.search(r"^  Location is heap block .* allocated by [^\n]*:\n((?:\s*#\d+ .*\n?)+)",
                         block, re.M)
    if not location:
        return False
    function, module = first_frame(location.group(1).splitlines())
    return function is not None and module.startswith("libQt6Gui") and function.startswith("QImageData::")


def qt_internal(block, stacks):
    """Both sides are Qt alone, or one side is Qt alone working on a QImage's
    pixels while the other is inside a library call on the same image.

    Qt has no frame pointers, so a pooled thread of ours that races on a
    QString through Qt calls can also look Qt-only. When the other side has
    frames of ours below the library call, only image work is let through: a
    QImage's pixel buffer, or both accesses inside libQt6Gui, where QImage's
    threaded conversions and QPainter's threaded fills run. Strings, byte
    arrays and containers live in libQt6Core and still fail. The residual
    risk is our own pooled code racing on a QImage it shares non-const, which
    implicit sharing makes rare."""
    if len(stacks) != 2:
        return False
    for one, other in (stacks, stacks[::-1]):
        if not qt_only(one):
            continue
        if qt_only(other):
            return True
        if made_by_library(other) and (allocated_as_qimage(block)
                                       or (library_module(one) == "libQt6Gui"
                                           and library_module(other) == "libQt6Gui")):
            return True
    return False


def library_module(frames):
    """The library of the first frame past the interceptors, without its
    version suffix: libQt6Gui for "libQt6Gui.so.6"."""
    _, module = first_frame(frames)
    return module.split(".so", 1)[0] if module else None


FREE = re.compile(r"^(operator delete|free)\b")


POSTED_EVENT_FREERS = ("QCoreApplicationPrivate::sendPostedEvents(",
                       "QCoreApplication::removePostedEvents(")


def posted_by_a_task(made):
    """The callout event a QtConcurrent task posts: from reportResultsReady,
    or from libQt6Core called straight by RunFunctionTaskBase::run(), which
    only calls Qt there to report that it finished."""
    allocator = made[1]
    if allocator[0].startswith("QFutureInterfaceBase::reportResultsReady("):
        return True
    return allocator[0] == "<null>" and allocator[1].startswith("libQt6Core") and \
        len(made) > 2 and made[2][0].startswith("QtConcurrent::RunFunctionTaskBase<") and \
        made[2][0].endswith(">::run()")


def qt_owned_memory(stacks):
    """Memory Qt allocates and frees itself on two threads, under a mutex in
    libQt6Core that TSan cannot see. Each side is an allocator interceptor
    called straight from Qt (or from libc called by Qt), and the side that
    frees has nothing of ours past that call:
    - a posted event: allocated where a task reports its result or that it
      finished (QFutureInterfaceBase), deleted once delivered by
      QCoreApplicationPrivate::sendPostedEvents or, undelivered, by
      QCoreApplication::removePostedEvents;
    - QThreadPool's queue and the runnables QtGui makes for its own threaded
      image work, freed by a pooled thread inside libQt6Core;
    - Qt Core's own calls into libc (strdup and free while handling the local
      time zone)."""
    if len(stacks) != 2:
        return False
    for freeing, allocating in (stacks, stacks[::-1]):
        freed = frame_functions(freeing)
        made = frame_functions(allocating)
        if len(freed) < 2 or len(made) < 2:
            continue
        if not FREE.match(freed[0][0]) or not ALLOCATION.match(made[0][0]):
            continue
        freer, allocator = freed[1], made[1]
        if freer[0].startswith(POSTED_EVENT_FREERS) and posted_by_a_task(made):
            return True
        # Not any allocation in libQt6Core: a QString of ours is allocated
        # there too, and its last reference dropped on a pooled thread is ours.
        if freer[1].startswith("libQt6Core") and qt_only(freeing) and (
                allocator[0].startswith("QThreadPoolPrivate::enqueueTask(")
                or allocator[1].startswith("libQt6Gui")):
            return True
        if freer[1].startswith("libc.so") and allocator[1].startswith("libc.so") and \
                len(freed) > 2 and len(made) > 2 and \
                freed[2][1].startswith("libQt6Core") and made[2][1].startswith("libQt6Core"):
            return True
    return False


def qt_descriptor_reuse(block, stacks):
    """A file descriptor number Qt closed and opened again (a QProcess pipe,
    an event dispatcher wakeup), with Qt making both calls. We never do I/O
    on Qt's descriptors ourselves."""
    if len(stacks) != 2 or not re.search(r"^  Location is file descriptor \d+", block, re.M):
        return False
    for frames in stacks:
        functions = frame_functions(frames)
        if len(functions) < 2 or not functions[1][1].startswith("libQt6"):
            return False
    return True


def pool_bookkeeping(block, stacks, threads):
    names = dict(THREAD.findall(block))
    if len(stacks) != 2 or any(names.get(thread) != POOL_THREAD for thread in threads):
        return False
    location = re.search(r"^  Location is heap block .* allocated by thread (T\d+):\n((?:\s*#\d+ .*\n?)+)",
                         block, re.M)
    if not location or names.get(location.group(1)) != POOL_THREAD:
        return False
    return all(only_qt_pool_frames(frames) for frames in stacks + [location.group(2).splitlines()])


QUEUED_CALL = re.compile(r"^QtPrivate::QCallableObject<.*>::impl\(")
CALL_BUILT = re.compile(r"^bool QMetaObject::invokeMethodCallableHelper<")
# A lambda's captures moved or copied into the queued call: "...'lambda'(...)::('lambda'(...)&&)".
LAMBDA_MOVE = re.compile(r"'lambda'.*::\('lambda'.*(?:&&| const&)\)$")


def delivered_queued_call(frames):
    """Running a functor queued with QMetaObject::invokeMethod, which Qt
    delivers through QObject::event on the receiver's thread."""
    functions = frame_functions(frames)
    for index, (function, module) in enumerate(functions):
        if function.startswith("QObject::event(") and module.startswith("libQt6Core"):
            return any(QUEUED_CALL.match(name) for name, _ in functions[:index])
    return False


def building_queued_call(frames):
    """The posting thread allocating the queued call or moving our captures
    into it, inside invokeMethod, with nothing else of ours on the way; or
    allocating a block (a payload the call carries) and touching it no more,
    since the latest access TSan kept is the allocation itself."""
    functions = frame_functions(frames)
    if not functions:
        return False
    if ALLOCATION.match(functions[0][0]):
        return True
    for index, (function, _) in enumerate(functions):
        if CALL_BUILT.match(function):
            return all(INTERCEPTOR.match(name) or name.startswith("QtPrivate::QCallableObject<")
                       or LAMBDA_MOVE.search(name) for name, _ in functions[:index])
    return False


def queued_call_handoff(stacks):
    """A functor queued from another thread with QMetaObject::invokeMethod
    reads what the posting thread built for it: the call itself, the captures
    moved into it, or a payload allocated before posting. Qt hands the call
    over through its posted-event queue mutex inside libQt6Core."""
    if len(stacks) != 2:
        return False
    return any(delivered_queued_call(receiver) and building_queued_call(poster)
               for receiver, poster in (stacks, stacks[::-1]))


def qt_lock_private(stacks):
    """QReadWriteLock's private part, which Qt allocates the first time two
    threads contend for the lock and publishes with an atomic exchange inside
    libQt6Core: a pthread mutex call straight from QBasicReadWriteLock on one
    side, its allocation straight from libQt6Core on the other."""
    if len(stacks) != 2:
        return False
    for lock, made in (stacks, stacks[::-1]):
        locking, allocating = frame_functions(lock), frame_functions(made)
        if len(locking) < 2 or len(allocating) < 2:
            continue
        if locking[0][0].startswith("pthread_mutex_") and \
                locking[1][0].startswith("QBasicReadWriteLock::") and \
                locking[1][1].startswith("libQt6Core") and \
                ALLOCATION.match(allocating[0][0]) and allocating[1][1].startswith("libQt6Core"):
            return True
    return False


def classify(block):
    if FATAL.search(block):
        return "fatal"
    stacks, threads = access_stacks(block)
    if not stacks:
        return "ours"  # Not a two-access race (double lock, signal, leak): look at it.
    names = dict(THREAD.findall(block))
    if any(names.get(thread) in KNOWN_QT_THREADS for thread in threads):
        return "external"
    if (task_start_handoff(stacks) or pool_bookkeeping(block, stacks, threads)
            or qt_internal(block, stacks) or qt_owned_memory(stacks)
            or qt_descriptor_reuse(block, stacks) or queued_call_handoff(stacks)
            or qt_lock_private(stacks)):
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

#!/usr/bin/env python3
"""Measure how busy the window thread is while a library's metadata is indexed.

Opens a library folder offscreen with a fresh profile, so every photo and
video is probed, and reads the window thread's CPU time from the moment
indexing starts to the moment it finishes. Parsing belongs on workers (#164);
--max-gui-percent (of one core, over the indexing pass) and
--max-gui-seconds turn it into a budget that fails the run. The share is the
steadier measure: a slower machine probes for longer, and the window thread's
seconds grow with it. The library is either --library or a generated one of --files files
(tests/make-performance-fixtures.py, which needs ImageMagick).

No personal files, settings, cache, instance socket or physical audio are used.
"""
import argparse
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time


def cpu_seconds(stat):
    fields = stat.read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def sample(binary, library, timeout, gpu=False):
    with tempfile.TemporaryDirectory(prefix="omaroll-indexing-") as root:
        scratch = Path(root)
        (scratch / "empty").mkdir()
        env = dict(os.environ)
        for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"):
            env[name] = str(scratch / name)
        for name in ("XDG_PICTURES_DIR", "OMARCHY_SCREENSHOT_DIR", "XDG_VIDEOS_DIR",
                     "XDG_DOWNLOAD_DIR", "OMARCHY_SCREENRECORD_DIR"):
            env[name] = str(scratch / "empty")
        env.update(QT_QPA_PLATFORMTHEME="", QT_QUICK_BACKEND="rhi", QSG_RHI_BACKEND="opengl",
                   QT_AUDIO_BACKEND="pulseaudio", PULSE_SERVER="unix:/nonexistent",
                   PIPEWIRE_REMOTE="omaroll-no-audio", QT_FORCE_STDERR_LOGGING="1",
                   OMAROLL_STARTUP_TRACE="1")
        if not gpu:
            env.update(QT_QPA_PLATFORM="offscreen", LIBGL_ALWAYS_SOFTWARE="1",
                       WAYLAND_DISPLAY=scratch.name)
        process = subprocess.Popen([str(binary), str(library)], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        # The main thread's task id is the process id.
        gui = Path(f"/proc/{process.pid}/task/{process.pid}/stat")
        whole = Path(f"/proc/{process.pid}/stat")
        marks = {}
        log = []
        pending = b""
        started = time.monotonic()
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stderr, selectors.EVENT_READ)
                while "index_done" not in marks:
                    if process.poll() is not None:
                        raise RuntimeError(f"Omaroll exited early: {process.returncode}\n"
                                           + "\n".join(log[-30:]))
                    if time.monotonic() - started > timeout:
                        raise RuntimeError(f"indexing did not finish in {timeout} s; saw {sorted(marks)}\n"
                                           + "\n".join(log[-30:]))
                    for key, _ in selector.select(timeout=0.05):
                        pending += os.read(key.fileobj.fileno(), 65536)
                        *lines, pending = pending.split(b"\n")
                        for line in lines:
                            text = line.decode(errors="replace")
                            log.append(text)
                            if "OMAROLL_STARTUP " not in text:
                                continue
                            stage = json.loads(text.split("OMAROLL_STARTUP ", 1)[1])["stage"]
                            if stage in ("index_start", "index_done") and stage not in marks:
                                marks[stage] = (time.monotonic(), cpu_seconds(gui), cpu_seconds(whole))
                    if "index_done" in marks and "index_start" not in marks:
                        raise RuntimeError("indexing finished without starting")
            (start, gui0, all0), (end, gui1, all1) = marks["index_start"], marks["index_done"]
            wall = end - start
            return dict(indexing_seconds=round(wall, 3),
                        gui_thread_cpu_seconds=round(gui1 - gui0, 3),
                        gui_thread_percent_one_core=round(100 * (gui1 - gui0) / wall, 1) if wall else 0,
                        process_cpu_seconds=round(all1 - all0, 3))
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--library", type=Path, help="Existing disposable media folder, read only")
    parser.add_argument("--files", type=int, default=10000, help="Size of the generated library")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--gpu", action="store_true",
                        help="Open a real window on the session's display and GPU; only inside a "
                             "disposable desktop such as omabox, never the active one")
    parser.add_argument("--timeout", type=float, default=600, help="Seconds allowed for one indexing pass")
    parser.add_argument("--max-gui-seconds", type=float,
                        help="Fail when the window thread spends more CPU than this while indexing (median)")
    parser.add_argument("--max-gui-percent", type=float,
                        help="Fail when the window thread uses more than this %% of one core while indexing (median)")
    args = parser.parse_args()
    if not 1 <= args.runs <= 10:
        parser.error("--runs must be 1..10")
    with tempfile.TemporaryDirectory(prefix="omaroll-indexing-library-") as root:
        library = args.library
        if library is None:
            if not 1 <= args.files <= 50000:
                parser.error("--files must be 1..50000")
            subprocess.run([sys.executable, str(Path(__file__).parent / "make-performance-fixtures.py"),
                            str(Path(root) / "fixtures"), "--files", str(args.files)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            library = Path(root) / "fixtures/library"
        elif not library.is_dir():
            parser.error("--library must be an existing folder")
        files = sum(1 for path in library.rglob("*") if path.is_file())
        results = [sample(args.binary.resolve(), library.resolve(), args.timeout, args.gpu)
                   for _ in range(args.runs)]
    gui = sorted(result["gui_thread_cpu_seconds"] for result in results)[len(results) // 2]
    share = sorted(result["gui_thread_percent_one_core"] for result in results)[len(results) // 2]
    print(json.dumps(dict(files=files, median_gui_thread_cpu_seconds=gui,
                          median_gui_thread_percent_one_core=share, runs=results), indent=2))
    print(f"benchmark_indexing: {files} files, window thread {gui} s of CPU, {share}% of one core, "
          f"while indexing (median of {len(results)})", file=sys.stderr)
    failed = []
    if args.max_gui_seconds is not None and gui > args.max_gui_seconds:
        failed.append(f"over the {args.max_gui_seconds} s budget")
    if args.max_gui_percent is not None and share > args.max_gui_percent:
        failed.append(f"over the {args.max_gui_percent}% budget")
    for message in failed:
        print("benchmark_indexing: " + message, file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

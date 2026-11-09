#!/usr/bin/env python3
"""Measure startup stages, media-ready frames and post-start resource use.

Offscreen OpenGL, with the actual driver recorded. Frames are submitted, not compositor-presented.
No personal files, settings, cache, instance socket or physical audio are used.

Three launches: a picture in the quick viewer (single-image), a generated
1080p H.264 clip in the quick viewer (single-video) and a folder in the
library. --budget MODE:STAGE=MS fails the run when the median of that stage,
in milliseconds from main(), is over the budget; --budget-scale multiplies
every budget for a slower machine.
"""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile
import time


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


EXPECTED = {"single-image": "image_frame", "single-video": "video_frame", "library": "grid_frame"}


def make_clip(path):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                    "testsrc2=size=1920x1080:rate=30", "-t", "4", "-c:v", "libx264",
                    "-preset", "ultrafast", "-pix_fmt", "yuv420p", str(path)], check=True)


def sample(binary, fixture, mode, file_count=100, library=None, clip=None):
    with tempfile.TemporaryDirectory(prefix="omaroll-benchmark-") as root:
        scratch = Path(root)
        pictures = library if library is not None else scratch / "Pictures"
        if library is None:
            pictures.mkdir()
        (scratch / "empty").mkdir()
        if library is None:
            for index in range(file_count):
                shutil.copyfile(fixture, pictures / f"image{index}{fixture.suffix}")
        env = dict(os.environ)
        for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
            env[name] = str(scratch / name)
        for name in ("XDG_PICTURES_DIR", "OMARCHY_SCREENSHOT_DIR"):
            env[name] = str(pictures)
        for name in ("XDG_VIDEOS_DIR", "XDG_DOWNLOAD_DIR", "OMARCHY_SCREENRECORD_DIR"):
            env[name] = str(scratch / "empty")
        env.update(QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="",
                   QT_QUICK_BACKEND="rhi", QSG_RHI_BACKEND="opengl",
                   LIBGL_ALWAYS_SOFTWARE="1", QT_AUDIO_BACKEND="pulseaudio",
                   PULSE_SERVER="unix:/nonexistent", PIPEWIRE_REMOTE="omaroll-no-audio",
                   WAYLAND_DISPLAY=scratch.name, QT_FORCE_STDERR_LOGGING="1",
                   OMAROLL_STARTUP_TRACE="1", QSG_INFO="1")
        args = [str(binary)]
        if mode == "single-image":
            args.append(str(fixture if library is not None else pictures / f"image0{fixture.suffix}"))
        elif mode == "single-video":
            args.append(str(clip))
            # As on CI: no GPU, so software decoding.
            env.setdefault("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",")
        started = time.perf_counter()
        process = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE)
        stages = {}
        observed_stages = {}
        diagnostics = deque(maxlen=30)
        graphics = None
        idle_start = None
        pending = b""
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stderr, selectors.EVENT_READ)
                expected = EXPECTED[mode]
                # At least eight seconds, for the idle sample; up to thirty on
                # a slow machine still waiting for its media frame.
                while (time.perf_counter() - started < 8
                       or (expected not in stages and time.perf_counter() - started < 30)):
                    if process.poll() is not None:
                        # Include both the partial line already read and stderr
                        # still buffered when the process exited.
                        remaining = pending + process.stderr.read()
                        diagnostics.extend(remaining.decode(errors="replace").splitlines())
                        raise RuntimeError(f"Omaroll exited early: {process.returncode}\n"
                                           + "\n".join(diagnostics))
                    if idle_start is None and time.perf_counter() - started >= 3:
                        idle_start = (time.perf_counter(), cpu_seconds(process.pid))
                    for key, _ in selector.select(timeout=0.05):
                        pending += os.read(key.fileobj.fileno(), 65536)
                        lines = pending.split(b"\n")
                        pending = lines.pop()
                        for line in lines:
                            diagnostics.append(line.decode(errors="replace"))
                            if b"OpenGL VENDOR: " in line:
                                graphics = line.split(b"OpenGL VENDOR: ", 1)[1].decode(errors="replace")
                            if b"OMAROLL_STARTUP " in line:
                                event = json.loads(line.split(b"OMAROLL_STARTUP ", 1)[1])
                                stages[event["stage"]] = event["elapsed_ms"]
                                observed_stages[event["stage"]] = round(
                                    (time.perf_counter() - started) * 1000, 3)
                idle_cpu = cpu_seconds(process.pid) - idle_start[1]
                idle_wall = time.perf_counter() - idle_start[0]
                status = Path(f"/proc/{process.pid}/status").read_text().splitlines()
                memory = {line.split(":")[0]: int(line.split()[1])
                          for line in status if line.startswith(("VmRSS:", "VmHWM:"))}
                required = {"application", "theme", "services", "qml", "first_frame", expected}
                if required - stages.keys() or graphics is None:
                    raise RuntimeError(
                        f"Missing startup evidence: {required - stages.keys()}; graphics={graphics}\n"
                        + "\n".join(diagnostics))
                return dict(mode=mode, graphics=graphics, main_entry_stages_ms=stages,
                            video=clip.name if mode == "single-video" else None,
                            observed_process_stages_ms=observed_stages,
                            fixture=fixture.name, fixture_bytes=fixture.stat().st_size,
                            fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                            files=file_count,
                            viewer_folder_file_entries=sum(1 for path in fixture.parent.iterdir() if path.is_file())
                            if library is not None and mode == "single-image" else file_count if mode == "single-image" else None,
                            fixture_mode="existing-library" if library is not None else "repeated-image",
                            app_cache_state="fresh", filesystem_cache_state="not-controlled",
                            post_start_cpu_percent_one_core=100 * idle_cpu / idle_wall,
                            cpu_observation_seconds=round(idle_wall, 3),
                            cpu_observation_note="from 3 seconds after process launch to the end of the sample (8 or more); background indexing may still be active",
                            rss_kib=memory["VmRSS"], peak_rss_kib=memory["VmHWM"])
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--fixture", type=Path,
                        default=Path(__file__).parent / "fixtures/viewer/transparent.png",
                        help="Still image to open; copied only when --library is omitted")
    parser.add_argument("--files", type=int, default=100)
    parser.add_argument("--library", type=Path,
                        help="Existing disposable mixed-media directory to scan read-only; ignores --files")
    parser.add_argument("--budget", action="append", default=[], metavar="MODE:STAGE=MS",
                        help="Fail when the median of STAGE in MODE is over MS from main(); repeatable")
    parser.add_argument("--budget-scale", type=float, default=1.0,
                        help="Multiply every budget, for a slower machine")
    args = parser.parse_args()
    budgets = []
    for text in args.budget:
        try:
            target, limit = text.split("=", 1)
            mode, stage = target.split(":", 1)
            budgets.append((mode, stage, float(limit) * args.budget_scale))
        except ValueError:
            parser.error(f"--budget must look like single-image:image_frame=900, not {text}")
        if mode not in EXPECTED:
            parser.error(f"--budget mode must be one of {', '.join(EXPECTED)}")
    if not 1 <= args.runs <= 20:
        parser.error("--runs must be 1..20")
    if args.library is None and not 1 <= args.files <= 10000:
        parser.error("--files must be 1..10000")
    if not args.fixture.is_file():
        parser.error("--fixture must be an existing image")
    library = args.library.resolve() if args.library is not None else None
    if library is not None:
        if not library.is_dir():
            parser.error("--library must be an existing media directory")
        if args.fixture.resolve().parent != library:
            parser.error("--fixture must be directly inside --library so the viewer scans that workload")
        args.files = sum(1 for path in library.rglob("*") if path.is_file())
        if not 1 <= args.files <= 50000:
            parser.error("--library must contain 1..50000 files")
    with tempfile.TemporaryDirectory(prefix="omaroll-startup-clip-") as root:
        clip = Path(root) / "clip-1080p.mp4"
        make_clip(clip)
        results = [sample(args.binary.resolve(), args.fixture.resolve(), mode, args.files, library, clip)
                   for mode in EXPECTED for _ in range(args.runs)]
    print(json.dumps(results, indent=2))
    failed = 0
    for mode, stage, limit in budgets:
        values = sorted(result["main_entry_stages_ms"].get(stage, float("inf"))
                        for result in results if result["mode"] == mode)
        median = values[len(values) // 2]
        verdict = "over" if median > limit else "within"
        print(f"benchmark_startup: {mode} {stage} median {median:.0f} ms, {verdict} the "
              f"{limit:.0f} ms budget", file=sys.stderr)
        failed += median > limit
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

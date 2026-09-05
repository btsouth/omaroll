#!/usr/bin/env python3
"""Measure startup stages, media-ready frames and idle resource use.

Offscreen OpenGL, with the actual driver recorded. Frames are submitted, not compositor-presented.
No personal files, settings, cache, instance socket or physical audio are used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import tempfile
import time


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def sample(binary, fixture, mode, file_count=100):
    with tempfile.TemporaryDirectory(prefix="omaroll-benchmark-") as root:
        scratch = Path(root)
        pictures = scratch / "Pictures"
        pictures.mkdir()
        (scratch / "empty").mkdir()
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
            args.append(str(pictures / f"image0{fixture.suffix}"))
        started = time.perf_counter()
        process = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE)
        stages = {}
        graphics = None
        idle_start = None
        pending = b""
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stderr, selectors.EVENT_READ)
                while time.perf_counter() - started < 8:
                    if process.poll() is not None:
                        raise RuntimeError(f"Omaroll exited early: {process.returncode}")
                    if idle_start is None and time.perf_counter() - started >= 3:
                        idle_start = (time.perf_counter(), cpu_seconds(process.pid))
                    for key, _ in selector.select(timeout=0.05):
                        pending += os.read(key.fileobj.fileno(), 65536)
                        lines = pending.split(b"\n")
                        pending = lines.pop()
                        for line in lines:
                            if b"OpenGL VENDOR: " in line:
                                graphics = line.split(b"OpenGL VENDOR: ", 1)[1].decode(errors="replace")
                            if b"OMAROLL_STARTUP " in line:
                                event = json.loads(line.split(b"OMAROLL_STARTUP ", 1)[1])
                                stages[event["stage"]] = event["elapsed_ms"]
                idle_cpu = cpu_seconds(process.pid) - idle_start[1]
                idle_wall = time.perf_counter() - idle_start[0]
                status = Path(f"/proc/{process.pid}/status").read_text().splitlines()
                memory = {line.split(":")[0]: int(line.split()[1])
                          for line in status if line.startswith(("VmRSS:", "VmHWM:"))}
                expected = "image_frame" if mode == "single-image" else "grid_frame"
                required = {"application", "theme", "services", "qml", "first_frame", expected}
                if required - stages.keys() or graphics is None:
                    raise RuntimeError(f"Missing startup evidence: {required - stages.keys()}; graphics={graphics}")
                return dict(mode=mode, graphics=graphics, main_entry_stages_ms=stages,
                            fixture=fixture.name, fixture_bytes=fixture.stat().st_size,
                            fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                            files=file_count,
                            idle_cpu_percent_one_core=100 * idle_cpu / idle_wall,
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
                        help="Still image to copy into the disposable library")
    parser.add_argument("--files", type=int, default=100)
    args = parser.parse_args()
    if not 1 <= args.runs <= 20:
        parser.error("--runs must be 1..20")
    if not 1 <= args.files <= 10000:
        parser.error("--files must be 1..10000")
    if not args.fixture.is_file():
        parser.error("--fixture must be an existing image")
    results = [sample(args.binary.resolve(), args.fixture.resolve(), mode, args.files)
               for mode in ("single-image", "library") for _ in range(args.runs)]
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Measure a real executable's first scene-graph frame and idle resource use.

Offscreen Mesa only. First frame does not prove the image or grid is ready.
No personal files, settings, cache, instance socket or physical audio are used.
"""
import argparse
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


def sample(binary, fixture, mode):
    with tempfile.TemporaryDirectory(prefix="omaroll-benchmark-") as root:
        scratch = Path(root)
        pictures = scratch / "Pictures"
        pictures.mkdir()
        (scratch / "empty").mkdir()
        for index in range(100):
            shutil.copyfile(fixture, pictures / f"image{index}.png")
        env = dict(os.environ)
        for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
            env[name] = str(scratch / name)
        for name in ("XDG_PICTURES_DIR", "OMARCHY_SCREENSHOT_DIR"):
            env[name] = str(pictures)
        for name in ("XDG_VIDEOS_DIR", "XDG_DOWNLOAD_DIR", "OMARCHY_SCREENRECORD_DIR"):
            env[name] = str(scratch / "empty")
        env.update(QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="",
                   QT_QUICK_BACKEND="rhi", QSG_RHI_BACKEND="opengl",
                   LIBGL_ALWAYS_SOFTWARE="1", PULSE_SERVER="unix:/nonexistent",
                   WAYLAND_DISPLAY=scratch.name, QT_FORCE_STDERR_LOGGING="1",
                   QSG_RENDER_TIMING="1")
        args = [str(binary)]
        if mode == "single-image":
            args.append(str(pictures / "image0.png"))
        started = time.perf_counter()
        process = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE)
        first_frame = None
        scene_seen = False
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
                            if b"time in renderer: total=" in line:
                                scene_seen = True
                            if scene_seen and first_frame is None and b"frame rendered in" in line:
                                first_frame = (time.perf_counter() - started) * 1000
                idle_cpu = cpu_seconds(process.pid) - idle_start[1]
                idle_wall = time.perf_counter() - idle_start[0]
                status = Path(f"/proc/{process.pid}/status").read_text().splitlines()
                memory = {line.split(":")[0]: int(line.split()[1])
                          for line in status if line.startswith(("VmRSS:", "VmHWM:"))}
                if first_frame is None:
                    raise RuntimeError("No scene-graph render timing received; check Mesa and Qt logging")
                return dict(mode=mode, first_scene_frame_ms=first_frame,
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
    args = parser.parse_args()
    if not 1 <= args.runs <= 20:
        parser.error("--runs must be 1..20")
    fixture = Path(__file__).parent / "fixtures/viewer/transparent.png"
    results = [sample(args.binary.resolve(), fixture, mode)
               for mode in ("single-image", "library") for _ in range(args.runs)]
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()

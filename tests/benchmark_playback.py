#!/usr/bin/env python3
"""Measure the CPU cost of playing a 4K video in the quick viewer.

Plays a generated clip (or --clip) offscreen and reports the process's CPU
time as a share of one core, its busiest threads and its minor page faults
over a fixed window after the first decoded frame. --max-cpu and
--max-faults turn the numbers into a budget that fails the run.

By default it runs offscreen with software rendering and decoding (Mesa
llvmpipe, FFmpeg), as on CI. --gpu opens a real window with the machine's
OpenGL driver and Omaroll's hardware decoder defaults, the path where Qt
copies every frame through system memory; run it inside a disposable desktop
such as omabox, never on the active one. Frames are submitted, not
compositor-presented, and audio is blocked.
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


def ticks(path):
    fields = path.read_text().rsplit(")", 1)[1].split()
    # minflt, utime, stime
    return int(fields[7]), int(fields[11]) + int(fields[12])


def threads(pid):
    result = {}
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            result[task.name] = ((task / "comm").read_text().strip(), ticks(task / "stat")[1])
        except OSError:
            pass
    return result


def make_clip(path, size, rate, seconds):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                    f"testsrc2=size={size}:rate={rate}", "-t", str(seconds),
                    "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", str(path)],
                   check=True)


def sample(binary, clip, gpu, settle, window):
    hz = os.sysconf("SC_CLK_TCK")
    with tempfile.TemporaryDirectory(prefix="omaroll-playback-") as root:
        scratch = Path(root)
        empty = scratch / "empty"
        empty.mkdir()
        env = dict(os.environ)
        for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"):
            env[name] = str(scratch / name)
        for name in ("XDG_PICTURES_DIR", "OMARCHY_SCREENSHOT_DIR", "XDG_VIDEOS_DIR",
                     "XDG_DOWNLOAD_DIR", "OMARCHY_SCREENRECORD_DIR"):
            env[name] = str(empty)
        env.update(QT_QPA_PLATFORMTHEME="", QT_QUICK_BACKEND="rhi", QSG_RHI_BACKEND="opengl",
                   QT_AUDIO_BACKEND="pulseaudio", PULSE_SERVER="unix:/nonexistent",
                   PIPEWIRE_REMOTE="omaroll-no-audio",
                   QT_FORCE_STDERR_LOGGING="1", OMAROLL_STARTUP_TRACE="1", QSG_INFO="1")
        if gpu:
            # Qt's offscreen platform has no hardware OpenGL without GLX, so
            # this draws a real window on the session's display.
            env.pop("LIBGL_ALWAYS_SOFTWARE", None)
        else:
            env.update(QT_QPA_PLATFORM="offscreen", WAYLAND_DISPLAY=scratch.name,
                       LIBGL_ALWAYS_SOFTWARE="1", QT_FFMPEG_DECODING_HW_DEVICE_TYPES=",")
        process = subprocess.Popen([str(binary), str(clip)], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        graphics = None
        first_frame = None
        pending = b""
        log = []
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stderr, selectors.EVENT_READ)
                deadline = time.monotonic() + 30
                while first_frame is None or time.monotonic() < first_frame + settle:
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("no decoded frame in time\n" + "\n".join(log[-30:]))
                    for key, _ in selector.select(timeout=0.05):
                        pending += os.read(key.fileobj.fileno(), 65536)
                        *lines, pending = pending.split(b"\n")
                        for line in lines:
                            text = line.decode(errors="replace")
                            log.append(text)
                            if "OpenGL VENDOR: " in text:
                                graphics = text.split("OpenGL VENDOR: ", 1)[1]
                            if "OMAROLL_STARTUP " in text and first_frame is None:
                                event = json.loads(text.split("OMAROLL_STARTUP ", 1)[1])
                                if event["stage"] == "video_decoded_frame":
                                    first_frame = time.monotonic()
                selector.unregister(process.stderr)
                stat = Path(f"/proc/{process.pid}/stat")
                faults0, cpu0 = ticks(stat)
                threads0 = threads(process.pid)
                started = time.monotonic()
                time.sleep(window)
                faults1, cpu1 = ticks(stat)
                threads1 = threads(process.pid)
                elapsed = time.monotonic() - started
            if process.poll() is not None:
                raise RuntimeError("Omaroll exited during the window")
            busiest = sorted(((threads1[tid][1] - threads0[tid][1], threads1[tid][0])
                              for tid in threads1.keys() & threads0.keys()), reverse=True)[:3]
            return dict(graphics=graphics, decoding="hardware defaults" if gpu else "software",
                        window_seconds=round(elapsed, 3),
                        cpu_percent_one_core=round(100 * (cpu1 - cpu0) / hz / elapsed, 1),
                        minor_faults_per_second=round((faults1 - faults0) / elapsed),
                        busiest_threads={name: round(100 * used / hz / elapsed, 1)
                                         for used, name in busiest})
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--clip", type=Path, help="Video to play; default a generated 4K H.264 clip")
    parser.add_argument("--rate", type=int, default=30, help="Frame rate of the generated clip")
    parser.add_argument("--gpu", action="store_true",
                        help="Use the machine's OpenGL driver and hardware decoders")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--settle", type=float, default=1.5,
                        help="Seconds after the first decoded frame before measuring")
    parser.add_argument("--window", type=float, default=4.0, help="Seconds measured")
    parser.add_argument("--max-cpu", type=float, help="Fail above this %% of one core (median)")
    parser.add_argument("--max-faults", type=float,
                        help="Fail above this many minor page faults per second (median)")
    args = parser.parse_args()
    if not 1 <= args.runs <= 20:
        parser.error("--runs must be 1..20")
    with tempfile.TemporaryDirectory(prefix="omaroll-clip-") as root:
        clip = args.clip
        if clip is None:
            clip = Path(root) / "clip-2160p.mp4"
            make_clip(clip, "3840x2160", args.rate, args.settle + args.window + 6)
        elif not clip.is_file():
            parser.error("--clip must be an existing video")
        results = [sample(args.binary.resolve(), clip.resolve(), args.gpu, args.settle, args.window)
                   for _ in range(args.runs)]
    cpu = sorted(result["cpu_percent_one_core"] for result in results)[len(results) // 2]
    faults = sorted(result["minor_faults_per_second"] for result in results)[len(results) // 2]
    print(json.dumps(dict(clip=str(args.clip or "generated 3840x2160 H.264 at %d fps" % args.rate),
                          median_cpu_percent_one_core=cpu, median_minor_faults_per_second=faults,
                          runs=results), indent=2))
    print(f"benchmark_playback: median {cpu}% of one core, {faults} minor faults/s, "
          f"{results[0]['graphics']}, {results[0]['decoding']} decoding", file=sys.stderr)
    failed = []
    if args.max_cpu is not None and cpu > args.max_cpu:
        failed.append(f"CPU {cpu}% of one core is over the {args.max_cpu}% budget")
    if args.max_faults is not None and faults > args.max_faults:
        failed.append(f"{faults} minor faults per second is over the {args.max_faults} budget")
    for message in failed:
        print("benchmark_playback: " + message, file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

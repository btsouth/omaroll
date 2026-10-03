#!/usr/bin/env python3
"""Create deterministic, disposable media fixtures for performance checks.

The generator uses bundled repository media and ImageMagick's ``magick``
command. It writes ``seeds/``, a flat hardlinked ``library/``, and
``manifest.json`` below one new or empty output directory. The photographs are
generated derivatives, not a real-camera dataset.

Example:
    python3 tests/make-performance-fixtures.py build/performance-fixtures \\
        --files 1000
"""

import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ElementTree


ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "resources/demo"
VIEWER = ROOT / "tests/fixtures/viewer"
ICONS = ROOT / "resources/icons"
PHOTO_LABEL = "generated-derived-photo-not-real-camera-dataset"

# Dimensions deliberately cover large landscape and portrait photographs while
# keeping the complete seed set small enough to generate on a devbox.
PHOTO_VARIANTS = (
    (6000, 4000, 84),
    (4000, 6000, 84),
    (4800, 3200, 82),
    (3200, 4800, 82),
    (4000, 3000, 84),
    (3000, 4000, 84),
    (3000, 2000, 80),
    (2000, 3000, 80),
    (2560, 1440, 82),
    (1440, 2560, 82),
    (2048, 1365, 86),
    (1365, 2048, 86),
    (1920, 1080, 78),
    (1080, 1920, 78),
    (1600, 1200, 88),
    (1200, 1600, 88),
    (1440, 900, 84),
    (900, 1440, 84),
    (1280, 720, 74),
    (720, 1280, 74),
    (800, 600, 90),
    (600, 800, 90),
    (512, 512, 88),
    (320, 240, 72),
)


def parser():
    command = argparse.ArgumentParser(
        description=__doc__.split("\n\n", 1)[0],
        epilog=(
            "Fixture creation is outside measurement. Library entries are hardlinks, "
            "so page cache and inode metadata are shared and results are not a "
            "cold-cache independent-copy benchmark."
        ),
    )
    command.add_argument("output", type=Path, metavar="OUTPUT",
                         help="new or empty directory to populate")
    command.add_argument("--files", type=int, default=1000, metavar="N",
                         help="library entries to create (default: 1000; range: 100..50000)")
    return command


def fail(message):
    raise SystemExit(f"error: {message}")


def relative(path):
    return path.relative_to(ROOT).as_posix()


def slug(text):
    result = "".join(character.lower() if character.isalnum() else "-"
                     for character in text)
    return "-".join(part for part in result.split("-") if part)


def validate_destination(path):
    if path.is_symlink():
        fail(f"output must not be a symlink: {path}")
    if path.exists():
        if not path.is_dir():
            fail(f"output exists and is not a directory: {path}")
        try:
            if any(path.iterdir()):
                fail(f"output directory is not empty: {path}")
        except OSError as error:
            fail(f"cannot inspect output directory {path}: {error}")
    return path.resolve()


def bundled_sources():
    photos = sorted(DEMO.glob("*.jpg"))
    videos = sorted(DEMO.glob("*.mp4"))
    copies = [
        ("alpha-png", VIEWER / "transparent.png", "alpha_png"),
        ("still-webp", VIEWER / "still.webp", "still_webp"),
        ("animated-gif", VIEWER / "animated.gif", "animated_gif"),
        ("animated-webp", VIEWER / "animated.webp", "animated_webp"),
        ("svg", ICONS / "omaroll.svg", "svg"),
    ]
    copies.extend(("mp4-" + path.stem, path, "mp4") for path in videos)
    copies.append(("matroska-" + (VIEWER / "tracks.mkv").stem,
                   VIEWER / "tracks.mkv", "matroska"))

    missing = []
    if not photos:
        missing.append(relative(DEMO / "*.jpg"))
    if not videos:
        missing.append(relative(DEMO / "*.mp4"))
    missing.extend(relative(path) for _, path, _ in copies if not path.is_file())
    if missing:
        fail("missing bundled source media: " + ", ".join(missing))
    return photos, copies


def require_magick():
    magick = shutil.which("magick")
    if magick is None:
        fail("ImageMagick 7 'magick' is required; install imagemagick")
    return magick


def run_magick(magick, arguments, description):
    try:
        subprocess.run([magick, *arguments], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", "") or getattr(error, "stdout", "") or str(error)
        fail(f"ImageMagick failed for {description}: {detail.strip()}")


def generate_photo(magick, source, destination, width, height, quality):
    run_magick(
        magick,
        [
            str(source),
            "-auto-orient",
            "-filter",
            "Lanczos",
            "-resize",
            f"{width}x{height}^",
            "-gravity",
            "center",
            "-extent",
            f"{width}x{height}",
            "-strip",
            "-quality",
            str(quality),
            str(destination),
        ],
        destination.name,
    )


def copy_file(source, destination):
    try:
        with source.open("rb") as source_file, destination.open("xb") as output:
            shutil.copyfileobj(source_file, output, length=1024 * 1024)
    except OSError as error:
        fail(f"cannot copy {relative(source)} to {destination.name}: {error}")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as media:
        for block in iter(lambda: media.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def dimensions(magick, path):
    if path.suffix.lower() == ".svg":
        try:
            root = ElementTree.parse(path).getroot()
            view_box = root.get("viewBox")
            width, height = (
                view_box.replace(",", " ").split()[2:4]
                if view_box
                else (root.get("width"), root.get("height"))
            )
            return int(float(width)), int(float(height))
        except (ElementTree.ParseError, TypeError, ValueError, IndexError) as error:
            fail(f"cannot inspect SVG {path.name}: {error}")
    try:
        output = subprocess.check_output(
            [magick, "identify", "-format", "%w %h\n", str(path)],
            text=True,
            stderr=subprocess.PIPE,
        )
        width, height = output.splitlines()[0].split()[:2]
        return int(width), int(height)
    except (OSError, subprocess.CalledProcessError, ValueError, IndexError) as error:
        detail = getattr(error, "stderr", "") or str(error)
        fail(f"ImageMagick cannot inspect {path.name}: {detail.strip()}")


def seed_record(path, index, kind, source, width, height, media_type,
                derived):
    seed_slug = path.stem.split("-", 1)[-1]
    return {
        "index": index,
        "path": f"seeds/{path.name}",
        "slug": seed_slug,
        "suffix": path.suffix.lower(),
        "kind": kind,
        "media_type": media_type,
        "source": relative(source),
        "derived": derived,
        "label": PHOTO_LABEL if derived else "bundled-copied-fixture",
        "bytes": path.stat().st_size,
        "width": width,
        "height": height,
        "sha256": sha256(path),
    }


def media_type(suffix):
    return {
        ".jpg": "jpeg",
        ".png": "png",
        ".webp": "webp",
        ".gif": "gif",
        ".svg": "svg",
        ".mp4": "mp4",
        ".mkv": "matroska",
    }.get(suffix, suffix.removeprefix("."))


def make_seeds(magick, seeds_directory, photos, copies):
    seeds = []
    for index, (width, height, quality) in enumerate(PHOTO_VARIANTS, start=1):
        source = photos[(index - 1) % len(photos)]
        name = (f"{index:02d}-generated-derived-photo-{width}x{height}-"
                f"{slug(source.stem)}.jpg")
        destination = seeds_directory / name
        generate_photo(magick, source, destination, width, height, quality)
        seeds.append(seed_record(destination, index, "generated_photo_jpeg",
                                 source, width, height, "jpeg", True))

    first = len(seeds) + 1
    for offset, (label, source, kind) in enumerate(copies):
        destination = seeds_directory / f"{first + offset:02d}-{label}{source.suffix}"
        copy_file(source, destination)
        width, height = dimensions(magick, destination)
        seeds.append(seed_record(
            destination,
            first + offset,
            f"copied_{kind}",
            source,
            width,
            height,
            media_type(source.suffix.lower()),
            False,
        ))
    return seeds


def populate_library(library, seeds, file_count):
    step = next(candidate for candidate in (7, 11, 17, 13, 5, 3, 2, 1)
                if math.gcd(candidate, len(seeds)) == 1)
    order = [(index * step) % len(seeds) for index in range(len(seeds))]
    media_counts = Counter()
    extension_counts = Counter()
    logical_bytes = 0

    for index in range(file_count):
        seed_index = order[index % len(order)]
        seed = seeds[seed_index]
        name = (f"media-{index + 1}-seed-{seed_index + 1:02d}-"
                f"{seed['slug']}{seed['suffix']}")
        source = library.parent / seed["path"]
        destination = library / name
        try:
            os.link(source, destination)
        except OSError as error:
            fail(
                f"cannot hardlink {source.name} to {destination.name}: {error}. "
                "The output filesystem must support hardlinks"
            )
        media_counts[seed["media_type"]] += 1
        extension_counts[seed["suffix"]] += 1
        logical_bytes += seed["bytes"]

    return media_counts, extension_counts, logical_bytes


def write_manifest(path, files, seeds, media_counts, extension_counts,
                   logical_bytes):
    seed_bytes = sum(seed["bytes"] for seed in seeds)
    manifest = {
        "schema_version": 1,
        "generator": "tests/make-performance-fixtures.py",
        "photo_label": PHOTO_LABEL,
        "source_policy": {
            "external_source": False,
            "sources": ["resources/demo", "tests/fixtures/viewer",
                        "resources/icons/omaroll.svg"],
        },
        "generation_outside_measurement": True,
        "library": {
            "path": "library",
            "flat": True,
            "files": files,
            "hardlinked": True,
            "logical_bytes": logical_bytes,
            "unique_seed_bytes": seed_bytes,
        },
        "file_counts": {
            "seeds": len(seeds),
            "library_files": files,
            "library_by_media_type": dict(sorted(media_counts.items())),
            "library_by_extension": dict(sorted(extension_counts.items())),
        },
        "seed_dimensions": "first frame or canvas dimensions",
        "seeds": seeds,
        "measurement_caveats": {
            "hardlinks": (
                "Library files are hardlinks to seeds. This saves disk space but "
                "shares page cache, inode metadata, and mtime."
            ),
            "filesystem_cache": (
                "Warm page cache can substantially change scan and thumbnail "
                "timings. Record and compare only equivalent cache conditions."
            ),
            "generation": (
                "Seed generation and hardlink creation are complete before the "
                "measured application run and must not be included in timings."
            ),
            "mtime": (
                "Library files are created only with os.link; no utime, copy2, "
                "or post-link metadata write is performed."
            ),
        },
    }
    with path.open("x", encoding="utf-8") as output:
        json.dump(manifest, output, indent=2, sort_keys=True)
        output.write("\n")


def main(argv=None):
    args = parser().parse_args(argv)
    if not 100 <= args.files <= 50000:
        parser().error("--files must be in the range 100..50000")

    output = validate_destination(args.output.expanduser())
    photos, copies = bundled_sources()
    magick = require_magick()

    try:
        output.mkdir(parents=True, exist_ok=True)
        seeds_directory = output / "seeds"
        library = output / "library"
        seeds_directory.mkdir()
        library.mkdir()
    except OSError as error:
        fail(f"cannot create fixture directories below {output}: {error}")

    seeds = make_seeds(magick, seeds_directory, photos, copies)
    media_counts, extension_counts, logical_bytes = populate_library(
        library, seeds, args.files
    )
    write_manifest(
        output / "manifest.json",
        args.files,
        seeds,
        media_counts,
        extension_counts,
        logical_bytes,
    )
    print(
        f"created {args.files} hardlinked library files and {len(seeds)} seeds\n"
        f"library: {library}\nmanifest: {output / 'manifest.json'}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()

# Omaroll development notes

Consult [current status](docs/STATUS.md) for planning or release work. Check the
latest published release when preparing publication, not on every edit.
PLAN.md is a historical design record, not the current work queue.

## Build and validate

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j4
bash tests/run-isolated.sh build/release
bash tests/run-isolated.sh build/release bash tests/run-opengl.sh build/release
```

Local checks require `bubblewrap`. Keep media tests, benchmarks and renders in
this sandbox. Blocking PulseAudio alone does not block native PipeWire.
Do not expose desktop audio, notification or clipboard services to tests.
Use [validation](tests/README.md) for sanitizer and acceptance requirements.

A deterministic render writes inside the sandbox's writable build directory:

```sh
bash tests/run-isolated.sh build/release build/release/omaroll \
  --render "$PWD/build/release/grid.png" --render-view grid
```

Use the OpenGL runner to verify video pixels. Do not drive the active desktop
for automated checks. Explicit physical desktop acceptance is a separate gate.

## Collaboration and publishing

- GitHub owner/account is `btsouth`, not `bts-cssi`. Use the personal account wrapper from the active global instructions.
- Preserve the app ID `io.github.tsouth89.omaroll`. The account rename did not
  change desktop, AppStream or compositor identifiers.
- Routine commits, branch pushes and PRs within requested implementation are
  authorized. Reuse existing merge/release authorization; opening a PR alone
  does not authorize a release. Use drafts only while code or public copy is unfinished.
- Inspect the exact PR title/body before posting and verify the live description,
  branch, diff and checks afterward. Public writing stays short and human.
- Access Linear through Toolport. The roadmap and remaining gates are in STATUS.md.

## Implementation rules

- C++20, Qt 6.8 or newer, Qt Quick, CMake/Ninja; MIT license.
- Check the installed Omarchy helpers and source before adding a feature.
  Annotation uses the configured editor, conversion/resize uses
  `omarchy-transcode` where suitable, and sharing uses the existing actions.
- Discovery is read-only. Explicit rename/Trash actions need their existing
  confirmations and organization updates. Image corrections save copies.
- Preserve media opacity and the app's Hyprland opacity opt-out. Theme colors,
  alpha, corner radius and font come from the existing theme integration.
- Thumbnails account for device pixel ratio. Test light/dark themes and narrow
  windows when changing layout; keep expensive work off the GUI thread.
- Do not claim headless frame submission proves physical audio or Wayland
  presentation. Recheck current release, PR state and CI before publication.

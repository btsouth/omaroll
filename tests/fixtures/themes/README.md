# Theme fixtures for the render matrix

Two palettes in Omarchy's `colors.toml` shape, written for these tests and
covered by the repository's MIT license. No third-party theme file is copied, so
CI can render both a dark and a light desktop without Omarchy installed.

Each directory holds what Omarchy puts in `~/.local/state/omarchy/current/theme`:

- `colors.toml`: the palette and `mode`, which every surface colour comes from.
- `shell.toml`: the `[launcher]` background and alpha that the app reads for its
  own surface transparency.
- `theme.name`: the display name shown in Settings.

`tests/run-opengl.sh` copies one of these over a scratch `HOME` and renders the
views from it, so the renders exercise the real theme path rather than the
built-in fallback palette. Add a palette here when a rendering bug depends on
one (a light theme, a very saturated accent, an opaque launcher).

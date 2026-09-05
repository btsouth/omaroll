# Contributing

Bug reports and focused pull requests are welcome. Before opening a pull request:

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
bash tests/run-isolated.sh build/release
```

Keep changes small, preserve the local-first design, and include a regression
test when fixing behavior that can be exercised without a desktop session.

Local tests require `bubblewrap`. Use the isolated runner for media tests and
renders, including sanitizer builds. See [validation](tests/README.md) for
OpenGL checks and [project status](docs/STATUS.md) before choosing new work.
Keep PRs in draft until their code and public description are ready; leave
session work open for review rather than merging it automatically.

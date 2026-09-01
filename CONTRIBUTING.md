# Contributing

Bug reports and focused pull requests are welcome. Before opening a pull request:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Keep changes small, preserve the local-first design, and include a regression
test when fixing behavior that can be exercised without a desktop session.

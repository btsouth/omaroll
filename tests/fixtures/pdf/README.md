# PDF fixtures

Original, generated files covered by the repository's MIT license, so the tests
need no downloads and no PDF writer that can draw a text layer.

- `text-page.pdf`: A4, one page, two text lines ("Omaroll selection" and
  "Second line") far enough apart to select one of them.
- `letter-pages.pdf`: Letter, two pages, one text line each, for the viewer's
  continuous list and the page-cell geometry checks.

The tests read these through `QFINDTESTDATA` rather than writing their own
pages: a build image without fonts draws no text layer, which made every text
assertion skip in CI while passing locally. Reading a committed file needs no
fonts, so the assertions run everywhere.

Regenerate with `generate.cpp`:

```sh
g++ -std=c++20 -fPIC -no-pie tests/fixtures/pdf/generate.cpp -o /tmp/mkpdf \
    $(pkg-config --cflags --libs Qt6Gui Qt6Core)
QT_QPA_PLATFORM=offscreen /tmp/mkpdf tests/fixtures/pdf
```

The word boxes the tests reason about come from `pdftotext -bbox` on these
files, so changing the text or its placement means rechecking the expectations
in `tests/tst_omaroll.cpp` and `tests/tst_ui.cpp`.

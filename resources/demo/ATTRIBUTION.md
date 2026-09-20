# Demo media

The 15 still images in this directory were generated for Omaroll's fictional
Quiet Horizons demo library. They contain no real people, brands, or landmarks.

The three video excerpts are dedicated to the public domain under CC0 1.0:

- [Timelapse of Clouds over Bellevue Canyon](https://commons.wikimedia.org/wiki/File:Timelapse_of_Clouds_over_Bellevue_Canyon.webm), by Extemporalist
- [Ocean surface waves](https://commons.wikimedia.org/wiki/File:Ocean_surface_waves.ogv), by Mostafameraji
- [Raindrops against the window in the night city, Las Palmas](https://commons.wikimedia.org/wiki/File:Raindrops_against_the_window_in_the_night_city,_Las_Palmas.webm)

The bundled excerpts were trimmed to five seconds, scaled to 1280 by 720,
encoded as H.264, and had their audio removed.

`quiet-horizons-guide.pdf` is generated for the demo library by
`tests/fixtures/pdf/generate.cpp`, which also writes the test fixtures:

```sh
g++ -std=c++20 -fPIC -no-pie tests/fixtures/pdf/generate.cpp -o /tmp/mkpdf \
    $(pkg-config --cflags --libs Qt6Gui Qt6Core)
QT_QPA_PLATFORM=offscreen /tmp/mkpdf tests/fixtures/pdf resources/demo
```

It is what the viewer's page list, find row and page controls are rendered and
reviewed against.

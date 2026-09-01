#pragma once

#include <QString>

// Builds a deterministic fictional library in a temporary directory.
//
// Two reasons this exists. Anyone can run `omaroll --demo` and see what the app
// does before pointing it at their own files, and every screenshot or video made
// for the project is shot against invented content rather than someone's real
// captures.
//
// Deterministic: the same seed produces the same library every run, so a
// screenshot taken today matches one taken next month.
namespace DemoLibrary {

struct Layout {
  QString root;
  QString pictures;
  QString videos;
};

// Generates the files and returns where they live. The directory is removed
// when the process exits.
[[nodiscard]] Layout build();

} // namespace DemoLibrary

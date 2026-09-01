#pragma once

#include <QString>

// Where omaroll looks, resolved from the environment so a customized Omarchy
// works with no configuration. Mirrors the fallback chain in
// omarchy-capture-screenshot and omarchy-capture-screenrecording exactly.
namespace CaptureLocations {

// $OMARCHY_SCREENSHOT_DIR -> $XDG_PICTURES_DIR -> ~/Pictures
[[nodiscard]] QString screenshots();

// $OMARCHY_SCREENRECORD_DIR -> $XDG_VIDEOS_DIR -> ~/Videos
[[nodiscard]] QString recordings();

[[nodiscard]] QString pictures();
[[nodiscard]] QString videos();
[[nodiscard]] QString downloads();

} // namespace CaptureLocations

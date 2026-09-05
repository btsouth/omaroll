#pragma once

#include <QtGlobal>

// PULSE_SERVER alone does not isolate Qt's native PipeWire backend. Apply this
// before constructing the application in tests and noninteractive renders.
inline void disableHeadlessAudio() {
  qputenv("QT_AUDIO_BACKEND", "pulseaudio");
  qputenv("PULSE_SERVER", "unix:/nonexistent");
  qputenv("PIPEWIRE_REMOTE", "omaroll-no-audio");
}

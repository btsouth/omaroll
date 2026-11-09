#pragma once

// The first video in a process pays for loading Qt's FFmpeg backend and for
// starting the GPU driver, and none of that depends on the file. On a direct
// video launch it runs on a worker while the window loads, rather than on the
// window thread once the player exists. Nothing here touches the file, the
// window or the audio, and a later video in the same process gains nothing.
namespace VideoWarmup {

// Starts the worker once; later calls do nothing.
void start();

// Whether the CUDA driver is usable, as VideoPlaybackEnvironment checks it.
// After start() this is the worker's answer, waiting for it if needed;
// otherwise the check runs here.
bool cudaDriverAvailable();

// True once start() has been called.
bool started();

} // namespace VideoWarmup

#pragma once

// The first OpenGL context a process creates pays for starting the driver.
// Qt Quick's threaded render loop creates a throwaway context on the window
// thread when a window is first exposed, only to read back its surface format
// (QRhiGles2InitParams::newFallbackSurface), and that thread is blocked until
// it is done. Creating and dropping a context on a worker while QML loads
// pays the driver's startup there instead. Nothing here touches a window.
namespace GraphicsWarmup {

// Whether this launch would gain from it: a Wayland session drawn through
// OpenGL on a platform that allows contexts on other threads.
bool wanted();

// Starts the worker once when wanted(); later calls do nothing.
void start();

} // namespace GraphicsWarmup

#include "app/VideoWarmup.h"

#include "app/StartupTrace.h"
#include "app/VideoPlayback.h"

#include <QFileInfo>
#include <QLibrary>
#include <QLibraryInfo>
#include <QThreadPool>

#include <atomic>
#include <future>
#include <mutex>

namespace {

struct State {
  std::once_flag once;
  std::atomic<bool> started{false};
  std::promise<bool> cuda;
  std::shared_future<bool> answer = cuda.get_future().share();
};

State& state() {
  static State instance;
  return instance;
}

void initializeCuda() {
  // The driver library stays loaded for the life of the process: unloading it
  // would undo cuInit, and FFmpeg loads the same library for the first video.
  static QLibrary* cuda = new QLibrary(QStringLiteral("cuda"), 1);
  if (!cuda->isLoaded() && !cuda->load()) return;
  using Init = int (*)(unsigned int);
  if (const auto init = reinterpret_cast<Init>(cuda->resolve("cuInit"))) {
    init(0);
  }
}

void loadBackend() {
  // Qt loads its multimedia plugin by this path; once it is resident, that
  // load is a lookup. The plugin stays loaded, as it would anyway.
  const QString path = QLibraryInfo::path(QLibraryInfo::PluginsPath) +
                       QStringLiteral("/multimedia/libffmpegmediaplugin.so");
  if (!QFileInfo::exists(path)) return;
  static QLibrary* plugin = new QLibrary(path);
  plugin->load();
}

void warm() {
  // The caller's environment, captured before anything changed it. The live
  // one is not read here: the window thread may be setting it.
  const QProcessEnvironment& inherited = videoPlaybackEnvironment().inherited();
  const QString key = QStringLiteral("QT_FFMPEG_DECODING_HW_DEVICE_TYPES");
  const bool chosenByCaller = inherited.contains(key);
  const bool cuda = !chosenByCaller && VideoPlaybackEnvironment::cudaDriverAvailable();
  state().cuda.set_value(cuda);
  // Unset, the decoder list puts CUDA first when the driver is there.
  const bool cudaFirst = chosenByCaller
      ? inherited.value(key).split(u',').first().trimmed() == u"cuda"
      : cuda;
  if (cudaFirst) initializeCuda();
  loadBackend();
  StartupTrace::note("warmup_done");
}

} // namespace

namespace VideoWarmup {

void start() {
  std::call_once(state().once, [] {
    state().started = true;
    QThreadPool::globalInstance()->start(warm);
  });
}

bool started() { return state().started; }

bool cudaDriverAvailable() {
  if (state().started) return state().answer.get();
  return VideoPlaybackEnvironment::cudaDriverAvailable();
}

} // namespace VideoWarmup

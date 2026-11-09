#include "actions/ClipboardServe.h"

#include "ext-data-control-v1-client-protocol.h"
#include "sources/ImageBudget.h"
#include "sources/PinnedImageReader.h"

#include <QBuffer>
#include <QFileInfo>
#include <QUrl>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>

#include <wayland-client.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <memory>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ClipboardServe {
namespace {

// Past this the file goes through wl-copy as before, without a PNG.
constexpr qint64 kMaxBytes = 512LL * 1024 * 1024;

struct State {
  wl_seat* seat = nullptr;
  ext_data_control_manager_v1* manager = nullptr;
  QByteArray type;
  // What a paste of that type gets, and the picture a PNG is made from: the
  // same bytes for a JPEG, or the file's own beside its URI.
  QByteArray bytes;
  QByteArray image;
  QStringList offered;
  bool cancelled = false;
  // Until the background copy takes over, paste requests wait here, so every
  // writer is that copy's own child and it can stop them.
  bool serving = false;
  std::vector<std::pair<int, bool>> queued;
  bool refusalLogged = false;
  // The compositor connection, which writers close: a writer outliving a
  // killed server must not keep it open.
  int displayFd = -1;
  // Paste writers still running, reaped as they finish.
  std::vector<pid_t> writers;
  // Says when a writer has finished, so it is reaped then and not left a
  // zombie until the next paste.
  int childFd = -1;
};

// A paste is written by a child of its own, which makes the PNG first when
// that is what was asked for, so neither a slow reader nor an encode ever
// holds up the next request. A writer that has not finished by the deadline
// is cut off, and past the cap a request is refused rather than piling up
// processes (each PNG writer holds a decoded picture).
constexpr std::size_t kMaxWriters = 4;
constexpr unsigned kWriteSeconds = 60;

// Descriptors the app let through to this process (not close-on-exec). Qt's
// and Wayland's own are close-on-exec, and stay.
void closeInherited(int keep) {
  DIR* directory = opendir("/proc/self/fd");
  if (!directory) return;
  std::vector<int> inherited;
  while (const dirent* entry = readdir(directory)) {
    const int fd = std::atoi(entry->d_name);
    if (fd <= STDERR_FILENO || fd == keep || fd == dirfd(directory)) continue;
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0 && !(flags & FD_CLOEXEC)) inherited.push_back(fd);
  }
  closedir(directory);
  for (const int fd : inherited) close(fd);
}

void reap(State& state) {
  std::erase_if(state.writers, [](pid_t writer) {
    return waitpid(writer, nullptr, WNOHANG) != 0;
  });
}

// Unreaped writers keep their pids, so none of these can be someone else's.
void stopWriters(State& state) {
  reap(state);
  for (const pid_t writer : state.writers) kill(writer, SIGTERM);
  for (const pid_t writer : state.writers) waitpid(writer, nullptr, 0);
  state.writers.clear();
}

void global(void* data, wl_registry* registry, uint32_t name, const char* interface,
            uint32_t version) {
  auto* state = static_cast<State*>(data);
  if (!state->seat && std::strcmp(interface, wl_seat_interface.name) == 0) {
    state->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
  } else if (!state->manager &&
             std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
    (void)version;
    state->manager = static_cast<ext_data_control_manager_v1*>(
        wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
  }
}
void globalRemove(void*, wl_registry*, uint32_t) {}
constexpr wl_registry_listener kRegistry{global, globalRemove};

// pending: requests still waiting for their own writers. Their descriptors
// are closed in this writer, or their readers would wait on it for EOF.
void deliver(State& state, int fd, bool png,
             const std::vector<std::pair<int, bool>>& pending = {}) {
  reap(state);
  if (state.writers.size() >= kMaxWriters) {
    if (!state.refusalLogged) {
      state.refusalLogged = true;
      qWarning("omaroll clipboard: %zu pastes are still being written; refusing more until one "
               "finishes", kMaxWriters);
    }
    close(fd);
    return;
  }
  const pid_t writer = fork();
  if (writer == 0) {
    for (const auto& [other, otherPng] : pending) {
      if (other != fd) close(other);
    }
    if (state.displayFd >= 0) close(state.displayFd);
    if (state.childFd >= 0) close(state.childFd);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    // This process never starts a thread of its own, so its children may
    // use Qt freely. The deadline holds even if the app ignored these signals.
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGALRM, SIG_DFL);
    alarm(kWriteSeconds);
    const QByteArray bytes = png ? encodePng(state.image, QString::fromUtf8(state.type)) : state.bytes;
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    const char* data = bytes.constData();
    qsizetype left = bytes.size();
    while (left > 0) {
      const ssize_t written = write(fd, data, size_t(left));
      if (written < 0) {
        if (errno == EINTR) continue;
        _exit(1);
      }
      data += written;
      left -= written;
    }
    _exit(bytes.isEmpty() ? 1 : 0);
  }
  if (writer > 0) state.writers.push_back(writer);
  close(fd);
}

void send(void* data, ext_data_control_source_v1*, const char* mimeType, int32_t fd) {
  auto* state = static_cast<State*>(data);
  const QByteArray type(mimeType);
  // The PNG is made from the picture; every other offered type gets the
  // served bytes (a file reference's text aliases get its URI).
  const bool png = type == "image/png" && type != state->type &&
                   state->offered.contains(u"image/png");
  if (!png && !state->offered.contains(QString::fromLatin1(type))) {
    close(fd);
  } else if (!state->serving) {
    state->queued.emplace_back(fd, png);
  } else {
    deliver(*state, fd, png);
  }
}

void dropQueued(State& state) {
  for (const auto& [fd, png] : state.queued) close(fd);
  state.queued.clear();
}
void cancelled(void* data, ext_data_control_source_v1*) {
  static_cast<State*>(data)->cancelled = true;
}
constexpr ext_data_control_source_v1_listener kSource{send, cancelled};

// Offers for the current selection, ours included, are never read.
void dataOffer(void*, ext_data_control_device_v1*, ext_data_control_offer_v1*) {}
void selection(void*, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
  if (offer) ext_data_control_offer_v1_destroy(offer);
}
void finished(void* data, ext_data_control_device_v1*) {
  static_cast<State*>(data)->cancelled = true;
}
constexpr ext_data_control_device_v1_listener kDevice{dataOffer, selection, finished, selection};

// wl_display_dispatch, plus reaping writers as they finish.
void serve(State& state, wl_display* display) {
  while (!state.cancelled) {
    while (wl_display_prepare_read(display) != 0) {
      if (wl_display_dispatch_pending(display) < 0) return;
    }
    if (state.cancelled) {
      wl_display_cancel_read(display);
      return;
    }
    const int flushed = wl_display_flush(display);
    if (flushed < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      return;
    }
    pollfd fds[2] = {{state.displayFd, short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0},
                     {state.childFd, POLLIN, 0}};
    // If signalfd is unavailable, still reap while the clipboard is idle.
    if (poll(fds, 2, state.childFd < 0 ? 100 : -1) < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR) continue;
      return;
    }
    if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
      if (wl_display_read_events(display) < 0) return;
    } else {
      wl_display_cancel_read(display);
    }
    if (fds[1].revents & POLLIN) {
      signalfd_siginfo info;
      while (read(state.childFd, &info, sizeof info) == sizeof info) {
      }
    }
    reap(state);
    if (wl_display_dispatch_pending(display) < 0) return;
  }
}

} // namespace

QByteArray uriList(const QString& path) {
  return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toEncoded() + "\r\n";
}

namespace {
// The listed format a served picture type is read as; empty for a file
// reference, whose bytes say what they are.
QByteArray readerFormat(const QString& servedType) {
  return servedType.startsWith(u"image/") ? ImageFormats::forMimeType(servedType) : QByteArray();
}
} // namespace

bool mayOfferPng(const QString& sourceType) {
  // PNG is itself; an SVG, compressed or not, has no size of its own to draw at.
  if (!sourceType.startsWith(u"image/") || sourceType == u"image/png" ||
      sourceType.startsWith(u"image/svg+xml")) {
    return false;
  }
  static const QList<QByteArray> readable = QImageReader::supportedMimeTypes();
  return readable.contains(sourceType.toLatin1());
}

QStringList offeredTypes(const QString& servedType, const QByteArray& bytes, qint64 maxPixels) {
  if (servedType == u"image/png") return {servedType};
  // A file reference also pastes as text, as wl-copy offered it: a terminal
  // or a plain text field gets the URI.
  QStringList served{servedType};
  if (servedType == QLatin1StringView(kUriList)) {
    for (const char* alias : kUriTextAliases) served << QString::fromLatin1(alias);
  }
  QBuffer buffer;
  buffer.setData(bytes);
  buffer.open(QIODevice::ReadOnly);
  // Pinned to what the type says, or for a file reference to what the bytes
  // are, among the formats Omaroll lists: no other plugin sees them.
  PinnedImageReader reader(&buffer, readerFormat(servedType));
  const ImageBudget::Estimate estimate = ImageBudget::estimate(reader);
  const qint64 pixels = qint64(estimate.stored.width()) * estimate.stored.height();
  const qint64 limit = ImageBudget::decodeLimitBytes();
  // Too big to decode, or to encode inside clipboard history's two seconds:
  // the picture goes on as itself only, as wl-copy put it before.
  if (!estimate.valid() || pixels <= 0 || pixels > maxPixels ||
      (limit > 0 && estimate.bytes > limit)) {
    return served;
  }
  return served << QStringLiteral("image/png");
}

QByteArray encodePng(const QByteArray& source, const QString& sourceType) {
  QBuffer input;
  input.setData(source);
  input.open(QIODevice::ReadOnly);
  PinnedImageReader reader(&input, readerFormat(sourceType));
  reader.setAutoTransform(true);
  const QImage image = reader.read();
  if (image.isNull()) return {};
  QByteArray png;
  QBuffer output(&png);
  output.open(QIODevice::WriteOnly);
  QImageWriter writer(&output, "png");
  // zlib level 1: a 24 megapixel photo in under a second.
  writer.setQuality(80);
  return writer.write(image) ? png : QByteArray();
}

int run(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: omaroll %s TYPE FILE\n", kOption);
    return 2;
  }
  // Image plugins (JPEG) load through the application's library paths, and
  // decodes get the same allocation limit as the app's. No threads start
  // here, so the forks below stay safe.
  QCoreApplication application(argc, argv);
  ImageBudget::applyStartupLimit();
  auto state = std::make_unique<State>();
  state->type = argv[2];
  QFile file(QFile::decodeName(argv[3]));
  if (!file.open(QIODevice::ReadOnly)) {
    std::fprintf(stderr, "Could not read %s\n", argv[3]);
    return 1;
  }
  if (file.size() > kMaxBytes) return kUnavailable;
  state->bytes = file.readAll();
  // A short read would put a cut-off picture on the clipboard.
  if (file.error() != QFileDevice::NoError || state->bytes.size() != file.size()) {
    std::fprintf(stderr, "Could not read all of %s\n", argv[3]);
    return 1;
  }
  file.close();
  state->image = state->bytes;
  // A file reference, with a PNG of the picture beside it.
  if (state->type == kUriList) state->bytes = uriList(file.fileName());

  // wayland-client would otherwise try wayland-0 in any session at all.
  if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_SOCKET")) {
    return kUnavailable;
  }
  wl_display* display = wl_display_connect(nullptr);
  if (!display) return kUnavailable;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistry, state.get());
  if (wl_display_roundtrip(display) < 0 || !state->seat || !state->manager) {
    if (state->manager) ext_data_control_manager_v1_destroy(state->manager);
    if (state->seat) wl_seat_destroy(state->seat);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return kUnavailable;
  }

  ext_data_control_source_v1* source =
      ext_data_control_manager_v1_create_data_source(state->manager);
  ext_data_control_source_v1_add_listener(source, &kSource, state.get());
  state->offered = offeredTypes(QString::fromUtf8(state->type), state->image);
  // Kept only to make the PNG from: without one, up to the size cap of a
  // file would otherwise sit here for as long as the copy is served.
  if (!state->offered.contains(u"image/png")) state->image = QByteArray();
  for (const QString& type : std::as_const(state->offered)) {
    ext_data_control_source_v1_offer(source, type.toUtf8().constData());
  }
  ext_data_control_device_v1* device =
      ext_data_control_manager_v1_get_data_device(state->manager, state->seat);
  ext_data_control_device_v1_add_listener(device, &kDevice, state.get());
  ext_data_control_device_v1_set_selection(device, source);
  if (wl_display_roundtrip(display) < 0) {
    std::fprintf(stderr, "The compositor did not take the selection\n");
    dropQueued(*state);
    wl_display_disconnect(display);
    return 1;
  }
  if (state->cancelled) {
    // Something else was copied in the meantime; that copy stands.
    dropQueued(*state);
    wl_display_disconnect(display);
    return kSuperseded;
  }

  // The selection is ours. The caller waits for this process, so hand it
  // success now and keep serving from a background copy.
  std::fflush(nullptr);
  const pid_t server = fork();
  if (server < 0) {
    dropQueued(*state);
    wl_display_disconnect(display);
    return 1;
  }
  if (server > 0) _exit(0);
  setsid();
  // Nothing of the app's stays open or busy: a folder on a USB stick it
  // was started from can still be unmounted.
  [[maybe_unused]] const int moved = chdir("/");
  const int null = open("/dev/null", O_RDWR);
  if (null >= 0) {
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    if (null > STDERR_FILENO) close(null);
  }
  state->displayFd = wl_display_get_fd(display);
  closeInherited(state->displayFd);
  std::signal(SIGPIPE, SIG_IGN);
  // Finished writers are told about here rather than through a handler, so
  // they are reaped in this loop alone and their pids stay ours until then.
  sigset_t children;
  sigemptyset(&children);
  sigaddset(&children, SIGCHLD);
  if (sigprocmask(SIG_BLOCK, &children, nullptr) == 0) {
    state->childFd = signalfd(-1, &children, SFD_CLOEXEC | SFD_NONBLOCK);
  }
  // Pastes asked for while the selection was being taken, now from here.
  state->serving = true;
  const std::vector<std::pair<int, bool>> queued = std::exchange(state->queued, {});
  for (std::size_t index = 0; index < queued.size(); ++index) {
    // Earlier ones are closed here already; each writer closes the later ones.
    deliver(*state, queued[index].first, queued[index].second,
            {queued.begin() + qsizetype(index) + 1, queued.end()});
  }
  serve(*state, display);
  stopWriters(*state);
  wl_display_disconnect(display);
  _exit(0);
}

} // namespace ClipboardServe

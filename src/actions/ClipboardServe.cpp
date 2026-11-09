#include "actions/ClipboardServe.h"

#include "ext-data-control-v1-client-protocol.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>

#include <wayland-client.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace ClipboardServe {
namespace {

// Past this the file goes through wl-copy as before, without a PNG.
constexpr qint64 kMaxBytes = 512LL * 1024 * 1024;

struct State {
  wl_seat* seat = nullptr;
  ext_data_control_manager_v1* manager = nullptr;
  QByteArray type;
  QByteArray bytes;
  QByteArray png;
  bool pngTried = false;
  bool cancelled = false;
};

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

// Each paste is written by a short-lived child, so a reader that stalls
// never stops this process from noticing that something else was copied.
void deliver(const QByteArray& bytes, int fd) {
  const pid_t writer = fork();
  if (writer == 0) {
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
    _exit(0);
  }
  close(fd);
}

void send(void* data, ext_data_control_source_v1*, const char* mimeType, int32_t fd) {
  auto* state = static_cast<State*>(data);
  const QByteArray type(mimeType);
  if (type == state->type) {
    deliver(state->bytes, fd);
  } else if (type == "image/png") {
    if (!state->pngTried) {
      state->pngTried = true;
      state->png = encodePng(state->bytes);
    }
    deliver(state->png, fd);
  } else {
    close(fd);
  }
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

} // namespace

QStringList offeredTypes(const QString& sourceType) {
  if (sourceType == u"image/jpeg") return {sourceType, QStringLiteral("image/png")};
  return {sourceType};
}

QByteArray encodePng(const QByteArray& source) {
  QBuffer input;
  input.setData(source);
  QImageReader reader(&input);
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
  auto state = std::make_unique<State>();
  state->type = argv[2];
  QFile file(QFile::decodeName(argv[3]));
  if (!file.open(QIODevice::ReadOnly)) {
    std::fprintf(stderr, "Could not read %s\n", argv[3]);
    return 1;
  }
  if (file.size() > kMaxBytes) return kUnavailable;
  state->bytes = file.readAll();
  file.close();

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
  for (const QString& type : offeredTypes(QString::fromUtf8(state->type))) {
    ext_data_control_source_v1_offer(source, type.toUtf8().constData());
  }
  ext_data_control_device_v1* device =
      ext_data_control_manager_v1_get_data_device(state->manager, state->seat);
  ext_data_control_device_v1_add_listener(device, &kDevice, state.get());
  ext_data_control_device_v1_set_selection(device, source);
  if (wl_display_roundtrip(display) < 0 || state->cancelled) {
    std::fprintf(stderr, "The compositor did not take the selection\n");
    wl_display_disconnect(display);
    return 1;
  }

  // The selection is ours. The caller waits for this process, so hand it
  // success now and keep serving from a background copy.
  std::fflush(nullptr);
  const pid_t server = fork();
  if (server < 0) {
    wl_display_disconnect(display);
    return 1;
  }
  if (server > 0) _exit(0);
  setsid();
  const int null = open("/dev/null", O_RDWR);
  if (null >= 0) {
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    if (null > STDERR_FILENO) close(null);
  }
  std::signal(SIGPIPE, SIG_IGN);
  // Writers are reaped as they exit.
  std::signal(SIGCHLD, SIG_IGN);
  // Image plugins (JPEG) load through the application's library paths.
  QCoreApplication application(argc, argv);
  while (!state->cancelled && wl_display_dispatch(display) >= 0) {
  }
  wl_display_disconnect(display);
  _exit(0);
}

} // namespace ClipboardServe

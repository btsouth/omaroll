#include "app/Diagnostics.h"
#include "theme/OmarchyTheme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QVariantMap>

namespace {

QString readLine(const QString& path, qsizetype limit = 200) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  return QString::fromUtf8(file.read(limit)).section(QLatin1Char('\n'), 0, 0).trimmed();
}

// Keeps a value to a short run of harmless characters.
QString bounded(QString value, const QString& allowed, qsizetype limit) {
  value.remove(QRegularExpression(QStringLiteral("[^%1]").arg(allowed)));
  return value.left(limit);
}

QString vendorName(const QString& id) {
  if (id == u"10de") return QStringLiteral("NVIDIA");
  if (id == u"1002") return QStringLiteral("AMD");
  if (id == u"8086") return QStringLiteral("Intel");
  if (id == u"1af4") return QStringLiteral("Virtio");
  if (id == u"15ad") return QStringLiteral("VMware");
  if (id == u"1234") return QStringLiteral("QEMU");
  return QStringLiteral("Other");
}

QString hexId(const QString& path) {
  QString value = readLine(path, 16).toLower();
  if (value.startsWith(u"0x")) value = value.mid(2);
  return bounded(value, QStringLiteral("0-9a-f"), 4);
}

} // namespace

namespace Diagnostics {

Sources Sources::system() {
  Sources sources;
  sources.omarchyPath = qEnvironmentVariable("OMARCHY_PATH");
  sources.stateHome = QDir::homePath() + QStringLiteral("/.local/state");
  sources.configHome = QDir::homePath() + QStringLiteral("/.config");
  sources.hasProgram = [](const QString& program) {
    return !QStandardPaths::findExecutable(program).isEmpty();
  };
  return sources;
}

const QList<Tool>& tools() {
  // One row per program Omaroll looks up, in the order a reader scans for
  // the common ones. Several rows can share a package.
  static const QList<Tool> list = {
      {QStringLiteral("tesseract"), QStringLiteral("Search and copy text in pictures"),
       QStringLiteral("tesseract tesseract-data-eng")},
      {QStringLiteral("ffmpeg"), QStringLiteral("Save a video frame"), QStringLiteral("ffmpeg")},
      {QStringLiteral("ffprobe"), QStringLiteral("Video details and dates"), QStringLiteral("ffmpeg")},
      {QStringLiteral("ffmpegthumbnailer"), QStringLiteral("Video thumbnails"),
       QStringLiteral("ffmpegthumbnailer")},
      {QStringLiteral("magick"), QStringLiteral("Photo dates and camera details"),
       QStringLiteral("imagemagick")},
      {QStringLiteral("pdftoppm"), QStringLiteral("PDF pages and thumbnails"), QStringLiteral("poppler")},
      {QStringLiteral("pdftotext"), QStringLiteral("Search and select PDF text"), QStringLiteral("poppler")},
      {QStringLiteral("jpegtran"), QStringLiteral("Lossless JPEG turns"), QStringLiteral("libjpeg-turbo")},
      {QStringLiteral("wl-copy"), QStringLiteral("Copy to the clipboard"), QStringLiteral("wl-clipboard")},
      {QStringLiteral("zbarimg"), QStringLiteral("Copy QR codes"), QStringLiteral("zbar")},
      {QStringLiteral("mpv"), QStringLiteral("Open in mpv"), QStringLiteral("mpv")},
      {QStringLiteral("omacut"), QStringLiteral("Trim recordings"), QStringLiteral("omacut")},
      {QStringLiteral("tensaku"), QStringLiteral("Annotate screenshots"), QStringLiteral("tensaku")},
      {QStringLiteral("omaframe"), QStringLiteral("Open in Omaframe"), QStringLiteral("omaframe")},
      {QStringLiteral("pinta"), QStringLiteral("Edit images"), QStringLiteral("pinta")},
      {QStringLiteral("imv"), QStringLiteral("View in imv"), QStringLiteral("imv")},
      {QStringLiteral("sushi"), QStringLiteral("Open PDF documents"), QStringLiteral("sushi")},
      {QStringLiteral("lp"), QStringLiteral("Print"), QStringLiteral("cups")},
      {QStringLiteral("localsend"), QStringLiteral("Send with LocalSend"), QStringLiteral("localsend")},
      {QStringLiteral("tailscale"), QStringLiteral("Send to your other machines"),
       QStringLiteral("tailscale")},
      {QStringLiteral("darktable"), QStringLiteral("Develop camera RAW"), QStringLiteral("darktable")},
      {QStringLiteral("rawtherapee"), QStringLiteral("Develop camera RAW"), QStringLiteral("rawtherapee")},
      {QStringLiteral("ART"), QStringLiteral("Develop camera RAW"), QStringLiteral("art")},
      {QStringLiteral("digikam"), QStringLiteral("Develop camera RAW"), QStringLiteral("digikam")},
      {QStringLiteral("gimp"), QStringLiteral("Develop camera RAW"), QStringLiteral("gimp")},
      {QStringLiteral("omarchy-transcode"), QStringLiteral("Convert and resize"), QStringLiteral("omarchy")},
      {QStringLiteral("omarchy-menu-share"), QStringLiteral("Send with LocalSend"), QStringLiteral("omarchy")},
      {QStringLiteral("omarchy-theme-bg-set"), QStringLiteral("Set as background"), QStringLiteral("omarchy")},
      {QStringLiteral("omarchy-tailscale-send"), QStringLiteral("Send to your other machines"),
       QStringLiteral("omarchy")},
  };
  return list;
}

QVariantList missing(const Sources& sources) {
  QStringList present;
  for (const Tool& tool : tools()) {
    if (sources.hasProgram && sources.hasProgram(tool.program)) present << tool.program;
  }
  // Any one RAW editor covers developing; suggest darktable only when none is there.
  const QStringList rawEditors = {QStringLiteral("darktable"), QStringLiteral("rawtherapee"),
                                  QStringLiteral("ART"), QStringLiteral("digikam"),
                                  QStringLiteral("gimp")};
  bool anyRawEditor = false;
  for (const QString& editor : rawEditors) anyRawEditor |= present.contains(editor);

  QVariantList rows;
  QStringList packages;
  for (const Tool& tool : tools()) {
    if (present.contains(tool.program) || tool.package == u"omarchy") continue;
    if (rawEditors.contains(tool.program) && (anyRawEditor || tool.program != u"darktable")) continue;
    if (packages.contains(tool.package)) continue;
    packages << tool.package;
    rows << QVariantMap{{QStringLiteral("program"), tool.program},
                        {QStringLiteral("feature"), tool.feature},
                        {QStringLiteral("package"), tool.package},
                        {QStringLiteral("install"), QStringLiteral("sudo pacman -S ") + tool.package}};
  }
  return rows;
}

QString packageVersion(const QString& pacmanLocal, const QString& name) {
  // Each installed package is a directory named name-pkgver-pkgrel, and
  // neither pkgver nor pkgrel contains a dash.
  const QString prefix = name + QLatin1Char('-');
  const QStringList entries =
      QDir(pacmanLocal).entryList({prefix + QLatin1Char('*')}, QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString& entry : entries) {
    const QString version = entry.mid(prefix.size());
    if (version.count(QLatin1Char('-')) == 1) {
      return bounded(version, QStringLiteral("0-9A-Za-z.:+_~-"), 60);
    }
  }
  return {};
}

QString omarchyVersion(const Sources& sources) {
  const QString path = sources.omarchyPath;
  if (!path.isEmpty() && QDir::cleanPath(path) != u"/usr/share/omarchy") {
    return QStringLiteral("development checkout");
  }
  // The edge channel installs omarchy-dev instead of omarchy.
  for (const QString& package : {QStringLiteral("omarchy-dev"), QStringLiteral("omarchy")}) {
    const QString version = packageVersion(sources.pacmanLocal, package);
    if (!version.isEmpty()) return version + QStringLiteral(" (") + package + QLatin1Char(')');
  }
  const QString file =
      readLine((path.isEmpty() ? QStringLiteral("/usr/share/omarchy") : path) + QStringLiteral("/version"), 60);
  const QString version = bounded(file, QStringLiteral("0-9A-Za-z.+_~-"), 60);
  return version.isEmpty() ? QStringLiteral("not found") : version;
}

QString hyprlandVersion(const Sources& sources) {
  for (const QString& package : {QStringLiteral("hyprland"), QStringLiteral("hyprland-git")}) {
    const QString version = packageVersion(sources.pacmanLocal, package);
    if (!version.isEmpty()) {
      return package == u"hyprland" ? version : version + QStringLiteral(" (") + package + QLatin1Char(')');
    }
  }
  return QStringLiteral("not installed");
}

QStringList graphics(const Sources& sources) {
  static const QRegularExpression card(QStringLiteral("^card\\d+$"));
  QStringList lines;
  const QStringList entries = QDir(sources.drm).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System,
                                                         QDir::Name);
  for (const QString& entry : entries) {
    if (!card.match(entry).hasMatch()) continue;
    const QString device = sources.drm + QLatin1Char('/') + entry + QStringLiteral("/device");
    const QString vendor = hexId(device + QStringLiteral("/vendor"));
    const QString model = hexId(device + QStringLiteral("/device"));
    if (vendor.isEmpty()) continue;
    const QString driver =
        bounded(QFileInfo(QFileInfo(device + QStringLiteral("/driver")).symLinkTarget()).fileName(),
                QStringLiteral("0-9A-Za-z_-"), 32);
    QString line = vendorName(vendor) + QStringLiteral(" (") + vendor + QLatin1Char(':') + model +
                   QLatin1Char(')');
    if (!driver.isEmpty()) {
      line += QStringLiteral(", driver ") + driver;
      const QString version = bounded(readLine(sources.modules + QLatin1Char('/') + driver +
                                               QStringLiteral("/version"), 60),
                                      QStringLiteral("0-9A-Za-z.+_~-"), 40);
      if (!version.isEmpty()) line += QLatin1Char(' ') + version;
    }
    if (!lines.contains(line)) lines << line;
  }
  return lines;
}

QString themeName(const Sources& sources) {
  // The directory the app's theme is read from, so the two never disagree.
  QString slug = readLine(OmarchyTheme::currentRootFor(sources.stateHome, sources.configHome) +
                              QStringLiteral("/theme.name"),
                          80);
  if (slug.isEmpty()) return QStringLiteral("not found");
  QStringList words = slug.replace(QLatin1Char('-'), QLatin1Char(' ')).split(QLatin1Char(' '), Qt::SkipEmptyParts);
  for (QString& word : words) word[0] = word.at(0).toUpper();
  return words.join(QLatin1Char(' '));
}

QString report(const Sources& sources, const QString& platform, const QString& theme, qreal scale) {
  const QString safeTheme = bounded(theme, QStringLiteral("a-zA-Z0-9 _().-"), 80);
  const QByteArray requestedBackend = qgetenv("QT_MEDIA_BACKEND");
  const QString backend = requestedBackend == "gstreamer" ? QStringLiteral("GStreamer")
                          : requestedBackend.isEmpty() || requestedBackend == "ffmpeg"
                              ? QStringLiteral("FFmpeg")
                              : QStringLiteral("custom Qt backend");
  const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE");
  const QString session = sessionType == "wayland" ? QStringLiteral("Wayland")
                          : sessionType == "x11"   ? QStringLiteral("X11")
                          : sessionType == "tty"   ? QStringLiteral("console")
                                                   : QStringLiteral("unknown");
  const bool hyprlandRunning = !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE");

  QStringList lines = {
      QStringLiteral("Omaroll: ") + QStringLiteral(OMAROLL_VERSION),
      QStringLiteral("Qt: ") + QString::fromLatin1(qVersion()),
      QStringLiteral("Platform: ") + platform + QStringLiteral(" / ") + QSysInfo::kernelType() +
          QLatin1Char(' ') + bounded(QSysInfo::kernelVersion(), QStringLiteral("0-9A-Za-z.+_~-"), 60),
      QStringLiteral("Session: ") + session +
          (hyprlandRunning ? QStringLiteral(", Hyprland running") : QString()),
      QStringLiteral("Omarchy: ") + omarchyVersion(sources),
      QStringLiteral("Hyprland: ") + hyprlandVersion(sources),
  };
  const QStringList adapters = graphics(sources);
  if (adapters.isEmpty()) {
    lines << QStringLiteral("GPU: not found");
  } else {
    for (const QString& adapter : adapters) lines << QStringLiteral("GPU: ") + adapter;
  }
  lines << QStringLiteral("Theme: ") + safeTheme;
  if (scale > 0) lines << QStringLiteral("Scale factor: ") + QString::number(scale);
  lines << QStringLiteral("Video decoding backend: Qt Multimedia ") + backend +
               QStringLiteral(" (actual hardware/software decoder depends on media)");
  for (const Tool& tool : tools()) {
    const bool found = sources.hasProgram && sources.hasProgram(tool.program);
    lines << tool.program + QStringLiteral(": ") +
                 (found ? QStringLiteral("present") : QStringLiteral("missing"));
  }
  return lines.join(QLatin1Char('\n'));
}

} // namespace Diagnostics

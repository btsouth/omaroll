#include "viewer/ViewerSession.h"

#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"
#include "sources/FileVersion.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QSet>
#include <QtConcurrent>

#include <algorithm>

namespace {
// What a still Image loads for |path|: the file, or a camera raw's preview.
QUrl imageUrl(const QString& path, const QString& version) {
  if (path.isEmpty()) return {};
  if (CameraRaw::isRawFile(path)) return CameraRaw::previewUrl(path, version);
  QUrl result = QUrl::fromLocalFile(path);
  result.setQuery(QStringLiteral("omaroll=") + version);
  return result;
}
} // namespace

ViewerSession::ViewerSession(QObject* parent) : QObject(parent) {
  // A burst of writes (a download landing, a batch export) settles before the
  // folder is read again.
  m_relist.setSingleShot(true);
  m_relist.setInterval(400);
  connect(&m_relist, &QTimer::timeout, this, [this] {
    if (!m_selection) {
      startListing();
    } else if (QFileInfo::exists(path())) {
      setSequence(m_paths, m_index);
    } else {
      forget(path());
    }
  });
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_relist.start(); });
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_relist.start(); });
  connect(&m_listing, &QFutureWatcher<QStringList>::finished, this, [this] {
    if (m_listedGeneration == m_generation) {
      applyListing(m_listing.result());
    }
  });
  connect(&m_preloadProbe, &QFutureWatcher<PreloadResult>::finished, this, [this] {
    m_preloadProbeRunning = false;
    const PreloadResult result = m_preloadProbe.result();
    if (result.generation != m_preloadGeneration) {
      startPreloadProbe();
      return;
    }
    std::array<QUrl, 2> urls;
    for (size_t i = 0; i < urls.size(); ++i) {
      const PreloadCandidate& candidate = result.candidates[i];
      // A replacement after the header was read must not inherit its budget
      // approval or cache identity. This checks metadata, never image headers.
      if (!candidate.path.isEmpty() && FileVersion::key(candidate.path) == candidate.version) {
        urls[i] = ::imageUrl(candidate.path, candidate.version);
      }
    }
    setPreloadUrls(urls);
  });
  connect(&m_rawSizeProbe, &QFutureWatcher<QSize>::finished, this, [this] {
    if (m_rawSizeVersion != m_contentVersion || !isRaw()) return;
    m_rawSize = m_rawSizeProbe.result();
    emit rawSizeChanged();
  });
}

ViewerSession::~ViewerSession() = default;

QUrl ViewerSession::url() const {
  const QString current = path();
  return current.isEmpty() ? QUrl() : QUrl::fromLocalFile(current);
}

QUrl ViewerSession::imageUrl() const { return ::imageUrl(path(), m_contentVersion); }

QString ViewerSession::fileName() const { return QFileInfo(path()).fileName(); }

QString ViewerSession::folder() const {
  const QString current = path();
  return current.isEmpty() ? QString() : QFileInfo(current).absolutePath();
}

bool ViewerSession::isVideo() const { return CaptureScanner::isVideo(m_mediaSuffix); }

bool ViewerSession::isRaw() const { return CameraRaw::isRaw(m_mediaSuffix); }

QUrl ViewerSession::rawUrl() const {
  return isRaw() ? CameraRaw::fullUrl(path(), m_contentVersion) : QUrl();
}

bool ViewerSession::isAnimated() const {
  // The same rule the library viewer uses: these two may hold several frames,
  // and AnimatedImage shows a single-frame file just as well.
  return m_mediaSuffix == u"gif" || m_mediaSuffix == u"webp";
}

void ViewerSession::open(const QStringList& paths) {
  if (paths.isEmpty()) {
    clear();
    return;
  }
  ++m_generation;
  m_relist.stop();
  if (paths.size() > 1) {
    m_selection = true;
    m_folder.clear();
    m_opened.clear();
    watchFolder({});
    setSequence(paths, 0);
    return;
  }
  m_selection = false;
  m_opened = paths.first();
  m_folder = QFileInfo(m_opened).absolutePath();
  watchFolder(m_folder);
  setSequence(paths, 0);
  startListing();
}

void ViewerSession::clear() {
  ++m_generation;
  m_relist.stop();
  m_folder.clear();
  m_opened.clear();
  m_selection = false;
  watchFolder({});
  setSequence({}, -1);
}

bool ViewerSession::step(int direction) {
  const int total = count();
  if (total < 2 || direction == 0) {
    return false;
  }
  setIndex((m_index + (direction < 0 ? -1 : 1) + total) % total);
  return true;
}

bool ViewerSession::jump(int index) {
  if (index < 0 || index >= count()) {
    return false;
  }
  setIndex(index);
  return true;
}

QUrl ViewerSession::neighbourUrl(int offset) const {
  const QString neighbour = neighbourPath(offset);
  return neighbour.isEmpty() ? QUrl() : QUrl::fromLocalFile(neighbour);
}

QString ViewerSession::neighbourPath(int offset) const {
  const int total = count();
  if (total < 2 || offset == 0) {
    return {};
  }
  const int index = ((m_index + offset) % total + total) % total;
  return index == m_index ? QString() : m_paths.at(index);
}

QUrl ViewerSession::neighbourImageUrl(int offset) const {
  const QString neighbour = neighbourPath(offset);
  return ::imageUrl(neighbour, FileVersion::key(neighbour));
}

bool ViewerSession::neighbourIsVideo(int offset) const {
  return CaptureScanner::isVideo(CaptureScanner::mediaSuffix(neighbourPath(offset)));
}

bool ViewerSession::neighbourIsAnimated(int offset) const {
  const QString suffix = CaptureScanner::mediaSuffix(neighbourPath(offset));
  return suffix == u"gif" || suffix == u"webp";
}

void ViewerSession::forget(const QString& path) {
  const int removed = int(m_paths.indexOf(path));
  if (removed < 0) {
    return;
  }
  QStringList remaining = m_paths;
  remaining.removeAt(removed);
  if (remaining.isEmpty()) {
    // Stop watching too, or a file landing in the folder later would refill
    // a viewer that has already closed.
    clear();
    emit emptied();
    return;
  }
  int next = m_index;
  if (removed < m_index) {
    --next;
  } else if (removed == m_index) {
    next = std::min(removed, int(remaining.size()) - 1);
  }
  setSequence(remaining, next);
}

void ViewerSession::openInLibrary() {
  const QString current = path();
  if (!current.isEmpty()) {
    emit libraryRequested(current);
  }
}

bool ViewerSession::isViewable(const QString& path) {
  const QString suffix = CaptureScanner::mediaSuffix(path);
  return CaptureScanner::isImage(suffix) || CaptureScanner::isVideo(suffix);
}

bool ViewerSession::canOpen(const QStringList& files) {
  return !files.isEmpty() && std::all_of(files.cbegin(), files.cend(), &ViewerSession::isViewable);
}

QStringList ViewerSession::siblings(const QString& folder, const QString& keep) {
  const QDir directory(folder);
  const QString kept = QFileInfo(keep).fileName();
  const bool keepHere = !keep.isEmpty() && QFileInfo(keep).absolutePath() == directory.absolutePath();
  QStringList names;
  for (const QString& name :
       directory.entryList(QDir::Files | QDir::Hidden | QDir::Readable, QDir::Unsorted)) {
    if (name.startsWith(QLatin1Char('.')) && !(keepHere && name == kept)) {
      continue;
    }
    const QString suffix = CaptureScanner::mediaSuffix(directory.filePath(name));
    if (CaptureScanner::isImage(suffix) || CaptureScanner::isVideo(suffix)) {
      names.append(name);
    }
  }
  if (keepHere && !names.contains(kept) && QFileInfo::exists(keep)) {
    names.append(kept);
  }

  // The order a file manager shows: "shot 2" before "shot 10", case folded,
  // with a plain comparison settling names the collator calls equal. Qt's
  // C-locale collator ignores numeric mode, so a minimal environment sorts as
  // English does, the way the library's name sort already does.
  QCollator collator;
  if (collator.locale().language() == QLocale::C) {
    collator.setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
  }
  collator.setNumericMode(true);
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  std::sort(names.begin(), names.end(), [&collator](const QString& left, const QString& right) {
    const int order = collator.compare(left, right);
    return order != 0 ? order < 0 : left < right;
  });

  QStringList paths;
  QSet<QString> seen;
  paths.reserve(names.size());
  const QString prefix = directory.absolutePath() + QLatin1Char('/');
  for (const QString& name : std::as_const(names)) {
    const QFileInfo file(prefix + name);
    const QString canonical = file.canonicalFilePath();
    const QString path = canonical.isEmpty() ? file.absoluteFilePath() : canonical;
    if (!seen.contains(path)) {
      seen.insert(path);
      paths.append(path);
    }
  }
  return paths;
}

QSize ViewerSession::preferredWindowSize(const QSize& available) {
  if (!available.isValid() || available.isEmpty()) {
    return QSize(1180, 780);
  }
  // The same normal window for every file: most of the screen, landscape,
  // between 4:3 and 16:9. A floating window cannot change shape once it is up,
  // so one shaped like the first picture squeezed every picture after it.
  const QSizeF room = QSizeF(available) * 0.8;
  const qreal aspect = std::clamp(room.width() / room.height(), 4.0 / 3.0, 16.0 / 9.0);
  QSizeF window(room.height() * aspect, room.height());
  if (window.width() > room.width()) {
    window = QSizeF(room.width(), room.width() / aspect);
  }
  return window.toSize().expandedTo(QSize(640, 480)).boundedTo(available);
}

void ViewerSession::setSequence(const QStringList& paths, int index) {
  const QString before = path();
  const int beforeIndex = m_index;
  const QString beforeSuffix = m_mediaSuffix;
  const double beforeStamp = m_stamp;
  const QString beforeVersion = m_contentVersion;
  const bool listChanged = paths != m_paths;
  m_paths = paths;
  m_index = paths.isEmpty() ? -1 : std::clamp(index, 0, int(paths.size()) - 1);
  if (listChanged) {
    emit sequenceChanged();
  }
  // A filename without an extension can change medium when replaced. Reopen
  // and directory relists must refresh it even when the sequence is unchanged.
  refreshDetails();
  if (path() != before || m_index != beforeIndex || m_mediaSuffix != beforeSuffix ||
      m_stamp != beforeStamp || m_contentVersion != beforeVersion) {
    emit currentChanged();
  }
  // Even an unchanged current file may have new or replaced neighbours.
  refreshPreloads();
}

void ViewerSession::setIndex(int index) {
  if (index == m_index) {
    return;
  }
  m_index = index;
  refreshDetails();
  emit currentChanged();
  refreshPreloads();
}

void ViewerSession::setPreloadUrls(const std::array<QUrl, 2>& urls) {
  if (urls == m_preloadUrls) return;
  m_preloadUrls = urls;
  emit preloadsChanged();
}

void ViewerSession::refreshPreloads() {
  ++m_preloadGeneration;
  m_pendingPreloadPaths = {neighbourPath(1), neighbourPath(-1)};
  // A two-file sequence wraps both directions onto the same picture. Also
  // exclude duplicate paths in an explicit selection, including the current.
  if (m_pendingPreloadPaths[0] == path()) m_pendingPreloadPaths[0].clear();
  if (m_pendingPreloadPaths[1] == path() || m_pendingPreloadPaths[1] == m_pendingPreloadPaths[0]) {
    m_pendingPreloadPaths[1].clear();
  }
  std::array<QUrl, 2> retained;
  for (size_t i = 0; i < retained.size(); ++i) {
    const QString& neighbour = m_pendingPreloadPaths[i];
    if (neighbour.isEmpty()) continue;
    for (const QUrl& approved : m_preloadUrls) {
      if (approved == ::imageUrl(neighbour, FileVersion::key(neighbour))) {
        retained[i] = approved;
        break;
      }
    }
  }
  // Keep only still-relevant approvals while fresh headers are being probed.
  // The displayed Image has already switched to its original-resolution URL.
  setPreloadUrls(retained);
  startPreloadProbe();
}

void ViewerSession::startPreloadProbe() {
  if (m_preloadProbeRunning ||
      (m_pendingPreloadPaths[0].isEmpty() && m_pendingPreloadPaths[1].isEmpty())) return;
  m_preloadProbeRunning = true;
  const auto paths = m_pendingPreloadPaths;
  const quint64 generation = m_preloadGeneration;
  m_preloadProbe.setFuture(QtConcurrent::run([paths, generation] {
    return probePreloads(paths, generation);
  }));
}

ViewerSession::PreloadResult ViewerSession::probePreloads(const std::array<QString, 2>& paths,
                                                         quint64 generation) {
  PreloadResult result;
  result.generation = generation;
  // A decoded-pixel estimate per session, not a process RSS limit. Decoder
  // scratch space, graphics resources and Qt's shared image cache are outside
  // this budget. Unknown formats reserve Qt's maximum 128 bits per pixel.
  constexpr quint64 budget = 64 * 1024 * 1024;
  quint64 remaining = budget;
  for (size_t i = 0; i < paths.size(); ++i) {
    const QString& path = paths[i];
    if (path.isEmpty()) continue;
    const QString version = FileVersion::key(path);
    if (version.isEmpty()) continue;
    const QString suffix = CaptureScanner::mediaSuffix(path);
    if (!CaptureScanner::isImage(suffix) || suffix == u"gif" || suffix == u"webp") continue;
    // A raw's preview decodes in a fraction of a second, and its header
    // reports the sensor's size rather than the preview's, so it would only
    // ever be budgeted wrongly.
    if (CameraRaw::isRaw(suffix)) continue;
    QImageReader reader(path);
    if (!reader.canRead() || reader.supportsAnimation()) continue;
    const QSize size = reader.size();
    if (size.width() <= 0 || size.height() <= 0) continue;
    const QImage::Format format = reader.imageFormat();
    const int depth = QImage::toPixelFormat(format).bitsPerPixel();
    const quint64 bytesPerPixel = depth <= 0 ? 16 : std::max(4, (depth + 7) / 8);
    // Compare before multiplying so oversized or hostile headers cannot wrap.
    const quint64 width = quint64(size.width());
    const quint64 height = quint64(size.height());
    if (width > remaining / bytesPerPixel / height) continue;
    if (FileVersion::key(path) != version) continue;
    remaining -= width * height * bytesPerPixel;
    result.candidates[i] = {path, version};
  }
  return result;
}

void ViewerSession::refreshDetails() {
  m_contentVersion = FileVersion::key(path());
  watchCurrentFile();
  m_mediaSuffix = CaptureScanner::mediaSuffix(path());
  const QFileInfo info(path());
  // Another file, or this one replaced: its raw size is read again.
  if (m_rawSizeVersion != m_contentVersion || !isRaw()) {
    const bool had = m_rawSize.isValid();
    m_rawSize = QSize();
    m_rawSizeVersion.clear();
    if (had) emit rawSizeChanged();
    if (isRaw() && !m_contentVersion.isEmpty()) {
      m_rawSizeVersion = m_contentVersion;
      m_rawSizeProbe.setFuture(
          QtConcurrent::run([file = path()] { return CameraRaw::fullSize(file); }));
    }
  }
  if (path().isEmpty() || !info.exists()) {
    m_stamp = 0;
    m_sizeLabel.clear();
    m_dateLabel.clear();
    return;
  }
  const QDateTime modified = info.lastModified();
  const QLocale locale = QLocale::system();
  m_stamp = double(modified.toMSecsSinceEpoch());
  m_sizeLabel = locale.formattedDataSize(info.size(), 1, QLocale::DataSizeTraditionalFormat);
  m_dateLabel = locale.toString(modified.date(), QStringLiteral("d MMMM yyyy")) +
                QStringLiteral("  ·  ") + locale.toString(modified.time(), QLocale::ShortFormat);
}

void ViewerSession::startListing() {
  if (m_folder.isEmpty()) {
    return;
  }
  const QString folder = m_folder;
  const QString keep = m_opened;
  m_listedGeneration = m_generation;
  m_listing.setFuture(QtConcurrent::run([folder, keep] { return siblings(folder, keep); }));
}

void ViewerSession::applyListing(const QStringList& listed) {
  const QString current = path();
  if (listed.isEmpty()) {
    // An unreadable folder still shows the file that was opened. A folder
    // that emptied under the window has nothing left to show.
    if (current.isEmpty() || !QFileInfo::exists(current)) {
      clear();
      emit emptied();
    }
    return;
  }
  const int found = int(listed.indexOf(current));
  if (found >= 0) {
    setSequence(listed, found);
    return;
  }
  // The file on screen went away: whatever now sits in its place shows.
  setSequence(listed, std::min(std::max(m_index, 0), int(listed.size()) - 1));
}

void ViewerSession::watchFolder(const QString& folder) {
  const QStringList watched = m_watcher.directories();
  if (watched == QStringList{folder}) {
    return;
  }
  if (!watched.isEmpty()) {
    m_watcher.removePaths(watched);
  }
  if (!folder.isEmpty()) {
    m_watcher.addPath(folder);
  }
}

void ViewerSession::watchCurrentFile() {
  const QStringList watched = m_watcher.files();
  const QString current = path();
  if (watched == QStringList{current}) return;
  if (!watched.isEmpty()) m_watcher.removePaths(watched);
  if (!current.isEmpty() && QFileInfo::exists(current)) m_watcher.addPath(current);
}

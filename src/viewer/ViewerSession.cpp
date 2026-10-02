#include "viewer/ViewerSession.h"

#include "sources/CaptureScanner.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QLocale>
#include <QtConcurrent>

#include <algorithm>

namespace {

QString suffixOf(const QString& name) {
  const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
  return dot < 0 ? QString() : name.mid(dot + 1);
}

} // namespace

ViewerSession::ViewerSession(QObject* parent) : QObject(parent) {
  // A burst of writes (a download landing, a batch export) settles before the
  // folder is read again.
  m_relist.setSingleShot(true);
  m_relist.setInterval(400);
  connect(&m_relist, &QTimer::timeout, this, &ViewerSession::startListing);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_relist.start(); });
  connect(&m_listing, &QFutureWatcher<QStringList>::finished, this, [this] {
    if (m_listedGeneration == m_generation) {
      applyListing(m_listing.result());
    }
  });
}

ViewerSession::~ViewerSession() = default;

QUrl ViewerSession::url() const {
  const QString current = path();
  return current.isEmpty() ? QUrl() : QUrl::fromLocalFile(current);
}

QString ViewerSession::fileName() const { return QFileInfo(path()).fileName(); }

QString ViewerSession::folder() const {
  const QString current = path();
  return current.isEmpty() ? QString() : QFileInfo(current).absolutePath();
}

bool ViewerSession::isVideo() const { return CaptureScanner::isVideo(suffixOf(path())); }

bool ViewerSession::isAnimated() const {
  // The same rule the library viewer uses: these two may hold several frames,
  // and AnimatedImage shows a single-frame file just as well.
  const QString suffix = suffixOf(path()).toLower();
  return suffix == u"gif" || suffix == u"webp";
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
  const int total = count();
  if (total < 2 || offset == 0) {
    return {};
  }
  const int index = ((m_index + offset) % total + total) % total;
  return index == m_index ? QUrl() : QUrl::fromLocalFile(m_paths.at(index));
}

bool ViewerSession::neighbourIsVideo(int offset) const {
  return CaptureScanner::isVideo(suffixOf(neighbourUrl(offset).toLocalFile()));
}

bool ViewerSession::neighbourIsAnimated(int offset) const {
  const QString suffix = suffixOf(neighbourUrl(offset).toLocalFile()).toLower();
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
    setSequence({}, -1);
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
  const QString suffix = suffixOf(QFileInfo(path).fileName());
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
    const QString suffix = suffixOf(name);
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
  paths.reserve(names.size());
  const QString prefix = directory.absolutePath() + QLatin1Char('/');
  for (const QString& name : std::as_const(names)) {
    paths.append(prefix + name);
  }
  return paths;
}

QSize ViewerSession::preferredWindowSize(const QString& path, const QSize& available,
                                         qreal devicePixelRatio) {
  const QSize minimum(640, 480);
  if (!available.isValid() || available.isEmpty()) {
    return QSize(1180, 780);
  }
  const QSizeF room = QSizeF(available) * 0.82;
  QSizeF media;
  if (!CaptureScanner::isVideo(suffixOf(path))) {
    QImageReader reader(path);
    QSize size = reader.size();
    if (size.isValid() && (reader.transformation() & QImageIOHandler::TransformationRotate90)) {
      size.transpose();
    }
    if (size.isValid() && !size.isEmpty()) {
      media = QSizeF(size) / std::max<qreal>(1, devicePixelRatio);
    }
  }
  if (media.isEmpty()) {
    media = QSizeF(16, 9).scaled(QSizeF(available) * 0.6, Qt::KeepAspectRatio);
  }
  if (media.width() > room.width() || media.height() > room.height()) {
    media = media.scaled(room, Qt::KeepAspectRatio);
  }
  return media.toSize().expandedTo(minimum).boundedTo(available);
}

void ViewerSession::setSequence(const QStringList& paths, int index) {
  const QString before = path();
  const int beforeIndex = m_index;
  const bool listChanged = paths != m_paths;
  m_paths = paths;
  m_index = paths.isEmpty() ? -1 : std::clamp(index, 0, int(paths.size()) - 1);
  if (listChanged) {
    emit sequenceChanged();
  }
  if (path() != before || m_index != beforeIndex) {
    refreshDetails();
    emit currentChanged();
  }
}

void ViewerSession::setIndex(int index) {
  if (index == m_index) {
    return;
  }
  m_index = index;
  refreshDetails();
  emit currentChanged();
}

void ViewerSession::refreshDetails() {
  const QFileInfo info(path());
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
      setSequence({}, -1);
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

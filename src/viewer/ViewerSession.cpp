#include "viewer/ViewerSession.h"

#include "actions/FileDrag.h"
#include "app/OpenRequest.h"

#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"
#include "sources/FileVersion.h"
#include "sources/ThumbnailSource.h"
#include "viewer/PictureDiagnosis.h"
#include "viewer/ViewerSort.h"
#include "sources/DecodeJournal.h"
#include "sources/PinnedImageReader.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QSet>
#include <QVariant>
#include <QtConcurrent>

#include <algorithm>
#include <memory>
#include <utility>

namespace {
// What a still Image loads for |path|: the file, or a camera raw's preview.
QUrl imageUrl(const QString& path, const QString& version) {
  if (path.isEmpty()) return {};
  if (CameraRaw::isRawFile(path)) return CameraRaw::previewUrl(path, version);
  QUrl result = QUrl::fromLocalFile(path);
  result.setQuery(QStringLiteral("omaroll=") + version);
  return result;
}
// Areas show the detail, so even a picture too long for a texture only needs
// a copy the size of the screen.
int copyEdge(const ImageBudget::Plan& plan) {
  return plan.areas ? ImageBudget::reducedEdge() : ImageBudget::reducedEdge(plan);
}
} // namespace

ViewerSession::ViewerSession(QObject* parent) : QObject(parent) {
  connect(this, &ViewerSession::currentChanged, this, &ViewerSession::deletionPathChanged);
  connect(this, &ViewerSession::currentChanged, this, &ViewerSession::pictureChanged);
  connect(this, &ViewerSession::sequenceRevisionChanged, this, &ViewerSession::deletionPathChanged);
  // A burst of writes (a download landing, a batch export) settles before the
  // folder is read again.
  m_relist.setSingleShot(true);
  m_relist.setInterval(400);
  connect(&m_relist, &QTimer::timeout, this, [this] {
    if (!m_selection) {
      startListing();
    } else {
      refreshSelection(path());
    }
  });
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_relist.start(); });
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_relist.start(); });
  connect(&m_listing, &QFutureWatcher<ListingResult>::finished, this, [this] {
    m_listingRunning = false;
    ListingResult result = m_listing.result();
    if (result.generation == m_generation && result.request == m_listingRequest && !m_selection) {
      applyListing(std::move(result));
    }
    if (m_listingPending) startListing();
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
        urls[i] = candidate.edge > 0
                      ? ImageBudget::reducedUrl(candidate.path, candidate.version, candidate.edge)
                  : candidate.estimate.direct
                      ? ::imageUrl(candidate.path, candidate.version)
                      : ImageBudget::pinnedUrl(candidate.path, candidate.version, candidate.estimate);
      }
    }
    m_neighbourPictures.clear();
    for (const PictureResult& picture : result.pictures) {
      if (!picture.path.isEmpty()) m_neighbourPictures.insert(picture.path, picture);
    }
    setPreloadUrls(urls);
  });
  connect(&m_pictureProbe, &QFutureWatcher<PictureResult>::finished, this, [this] {
    m_pictureProbeRunning = false;
    const PictureResult result = m_pictureProbe.result();
    if (m_pictureKnown || m_picturePath.isEmpty()) return;
    if (result.path != m_picturePath || result.version != m_pictureVersion) {
      startPictureProbe();
      return;
    }
    applyPicture(result);
    emit pictureChanged();
  });
  const auto applyCheck = [this](const CheckResult& result, bool failure) {
    if (result.path != path() || result.version != m_contentVersion) return;
    if (failure) {
      if (m_pictureFailure == result.failure) return;
      m_pictureFailure = result.failure;
    } else {
      if (m_pictureWarning == result.warning) return;
      m_pictureWarning = result.warning;
    }
    emit pictureCheckChanged();
  };
  connect(&m_warningCheck, &QFutureWatcher<CheckResult>::finished, this,
          [this, applyCheck] { applyCheck(m_warningCheck.result(), false); });
  connect(&m_failureCheck, &QFutureWatcher<CheckResult>::finished, this,
          [this, applyCheck] { applyCheck(m_failureCheck.result(), true); });
  connect(&m_rawSizeProbe, &QFutureWatcher<RawSizeResult>::finished, this, [this] {
    m_rawSizeProbeRunning = false;
    const RawSizeResult result = m_rawSizeProbe.result();
    if (result.path == path() && result.version == m_contentVersion && isRaw() &&
        FileVersion::key(result.path) == result.version) {
      m_rawSize = result.size;
      emit rawSizeChanged();
    }
    if (result.path != m_rawSizePath || result.version != m_rawSizeVersion) startRawSizeProbe();
  });
}

ViewerSession::~ViewerSession() = default;

QUrl ViewerSession::url() const {
  const QString current = path();
  return current.isEmpty() ? QUrl() : QUrl::fromLocalFile(current);
}

QUrl ViewerSession::imageUrl() const {
  if (!isPlainStill()) return ::imageUrl(path(), m_contentVersion);
  // Nothing to load for a picture that cannot be decoded at all; the viewer
  // shows pictureError instead.
  if (!m_pictureKnown || !m_plan.error.isEmpty()) return {};
  if (m_plan.reduced) return ImageBudget::reducedUrl(path(), m_contentVersion, m_reducedEdge);
  return stillUrl();
}

QUrl ViewerSession::stillUrl() const {
  // Contents another format than the name says: Qt's own file loading would
  // offer them to every installed plugin, so they are read pinned instead.
  return m_pictureEstimate.direct ? ::imageUrl(path(), m_contentVersion)
                                  : ImageBudget::pinnedUrl(path(), m_contentVersion, m_pictureEstimate);
}

QUrl ViewerSession::detailUrl() const {
  return reduced() && m_plan.detail && !m_plan.areas ? stillUrl() : QUrl();
}

QUrl ViewerSession::areaUrl(const QRect& area, const QSize& size) const {
  return areas() ? ImageBudget::areaUrl(path(), m_contentVersion, area, size) : QUrl();
}

bool ViewerSession::isPlainStill() const {
  return !path().isEmpty() && CaptureScanner::isImage(m_mediaSuffix) && !isRaw() && !isAnimated();
}

void ViewerSession::refreshPicture() {
  if (!isPlainStill()) {
    m_pictureKnown = false;
    m_plan = {};
    m_pictureEstimate = {};
    m_picturePath.clear();
    m_pictureVersion.clear();
    return;
  }
  if (m_picturePath == path() && m_pictureVersion == m_contentVersion) return;
  m_picturePath = path();
  m_pictureVersion = m_contentVersion;
  m_pictureKnown = false;
  m_plan = {};
  m_pictureEstimate = {};
  const auto known = m_neighbourPictures.constFind(m_picturePath);
  if (known != m_neighbourPictures.constEnd() && !m_pictureVersion.isEmpty() &&
      known->version == m_pictureVersion) {
    applyPicture(*known);
    return;
  }
  startPictureProbe();
}

// Only JPEG and PNG say where they end; a cut-off one still decodes.
void ViewerSession::startPictureCheck() {
  if (m_checkedPath == path() && m_checkedVersion == m_contentVersion) return;
  m_checkedPath = path();
  m_checkedVersion = m_contentVersion;
  const bool cleared = !m_pictureWarning.isEmpty() || !m_pictureFailure.isEmpty();
  m_pictureWarning.clear();
  m_pictureFailure.clear();
  if (cleared) emit pictureCheckChanged();
  if (m_checkedPath.isEmpty() || isVideo() || isRaw() ||
      !(m_mediaSuffix == u"jpg" || m_mediaSuffix == u"jpeg" || m_mediaSuffix == u"jpe" ||
        m_mediaSuffix == u"jfif" || m_mediaSuffix == u"png")) {
    return;
  }
  const QString file = m_checkedPath;
  const QString version = m_checkedVersion;
  m_warningCheck.setFuture(QtConcurrent::run([file, version] {
    return CheckResult{file, version, PictureDiagnosis::truncation(file), {}};
  }));
}

void ViewerSession::diagnosePicture() {
  if (path().isEmpty() || isVideo()) return;
  const QString file = path();
  const QString version = m_contentVersion;
  if (!m_pictureFailure.isEmpty() && m_checkedPath == file && m_checkedVersion == version) return;
  m_failureCheck.setFuture(QtConcurrent::run([file, version] {
    return CheckResult{file, version, {}, PictureDiagnosis::failure(file).toMap()};
  }));
}

void ViewerSession::diagnoseVideo(const QString& playerError) {
  if (path().isEmpty() || !isVideo()) return;
  const QString file = path();
  const QString version = m_contentVersion;
  if (!m_pictureFailure.isEmpty() && m_checkedPath == file && m_checkedVersion == version) return;
  m_failureCheck.setFuture(QtConcurrent::run([file, version, playerError] {
    return CheckResult{file, version, {}, PictureDiagnosis::videoFailure(file, playerError).toMap()};
  }));
}

void ViewerSession::startPictureProbe() {
  if (m_pictureProbeRunning || m_pictureKnown || m_picturePath.isEmpty()) return;
  m_pictureProbeRunning = true;
  const QString file = m_picturePath;
  const QString version = m_pictureVersion;
  m_pictureProbe.setFuture(QtConcurrent::run([file, version] {
    // A replacement since the version was taken is read again once the
    // folder watcher reports it.
    return PictureResult{file, version,
                         FileVersion::key(file) == version ? ImageBudget::estimate(file)
                                                           : ImageBudget::Estimate{}};
  }));
}

void ViewerSession::applyPicture(const PictureResult& result) {
  m_pictureKnown = true;
  m_pictureEstimate = result.estimate;
  m_plan = ImageBudget::plan(result.estimate, ImageBudget::reducedEdge());
  m_reducedEdge = m_plan.reduced ? copyEdge(m_plan) : 0;
}

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
  return m_animated;
}

void ViewerSession::setStartPosition(const QString& path, qint64 milliseconds) {
  m_startPath = milliseconds >= 0 ? path : QString();
  m_startMs = milliseconds >= 0 ? milliseconds : -1;
  emit startPositionChanged();
}

qint64 ViewerSession::takeStartPosition(const QString& path) {
  const qint64 result = pendingStartPosition(path);
  if (result >= 0) {
    m_startPath.clear();
    m_startMs = -1;
  }
  return result;
}

void ViewerSession::open(const QStringList& paths) {
  if (!m_startPath.isEmpty() && !paths.contains(m_startPath)) {
    m_startPath.clear();
    m_startMs = -1;
  }
  if (paths.isEmpty()) {
    clear();
    return;
  }
  ++m_generation;
  m_folderPaths.clear();
  m_deletionPaths.clear();
  m_explicitEntryPaths.clear();
  m_companions.clear();
  m_listingPending = false;
  m_relist.stop();
  ++m_openSerial;
  if (paths.size() > 1) {
    m_selection = true;
    m_folder.clear();
    m_opened.clear();
    setSequence(paths, 0);
    watchSelectionFolders();
    return;
  }
  m_selection = false;
  m_opened = paths.first();
  m_folder = QFileInfo(m_opened).absolutePath();
  watchFolder(m_folder);
  setSequence(paths, 0);
  startListing();
}

void ViewerSession::setDeletionPaths(const QHash<QString, QString>& entryPaths) {
  for (auto it = entryPaths.cbegin(); it != entryPaths.cend(); ++it) {
    m_explicitEntryPaths.insert(it.key(), it.value());
  }
  ++m_sequenceRevision;
  emit sequenceRevisionChanged();
}

void ViewerSession::clear() {
  ++m_generation;
  m_startPath.clear();
  m_startMs = -1;
  m_folderPaths.clear();
  m_deletionPaths.clear();
  m_explicitEntryPaths.clear();
  m_companions.clear();
  m_listingPending = false;
  m_relist.stop();
  ++m_openSerial;
  m_folder.clear();
  m_opened.clear();
  m_selection = false;
  watchFolder({});
  setSequence({}, -1);
}

QString ViewerSession::companionPath() const { return companionPathAt(m_index); }

QString ViewerSession::companionPathAt(int index) const {
  if (!m_pairRawJpeg || m_selection || index < 0 || index >= count()) return {};
  return m_companions.value(m_paths.at(index));
}

QString ViewerSession::rawFormatAt(int index) const {
  if (index < 0 || index >= count()) return {};
  const QString& file = m_paths.at(index);
  return CameraRaw::isRawFile(file) ? QFileInfo(file).suffix().toUpper() : QString();
}

void ViewerSession::setPairRawJpeg(bool value) {
  if (m_pairRawJpeg == value) return;
  m_pairRawJpeg = value;
  if (!m_selection && !m_folderPaths.isEmpty()) rebuildFolderSequence(path());
  else {
    ++m_sequenceRevision;
    emit sequenceRevisionChanged();
    emit companionPathChanged();
  }
  emit pairRawJpegChanged();
}

void ViewerSession::setSortOrder(const QString& order) {
  if (!ViewerSort::isKnown(order) || m_sortOrder == order) return;
  m_sortOrder = order;
  emit sortChanged();
  // Relisted in the new order; the file on screen stays the current one.
  if (!m_selection) startListing();
}

void ViewerSession::setSortReversed(bool reversed) {
  if (m_sortReversed == reversed) return;
  m_sortReversed = reversed;
  emit sortChanged();
  if (!m_selection) startListing();
}

bool ViewerSession::switchCompanion() {
  const QString companion = companionPath();
  if (companion.isEmpty()) return false;
  if (!QFileInfo::exists(companion)) {
    startListing();
    return false;
  }
  // Replace this pair's representative only. Actions keep using path(), so
  // switching never broadens Trash/rename to include the other physical file.
  QStringList paths = m_paths;
  paths[m_index] = companion;
  m_opened = companion;
  setSequence(paths, m_index);
  return true;
}

bool ViewerSession::step(int direction) {
  if (m_selection && direction != 0) {
    const QString before = path();
    QString preferred;
    for (int offset = 1; offset <= count(); ++offset) {
      const int index = (m_index + (direction < 0 ? -offset : offset) + count()) % count();
      const QString candidate = m_paths.at(index);
      const QFileInfo info(candidate);
      if (info.isFile() && info.isReadable()) {
        preferred = candidate;
        break;
      }
    }
    refreshSelection(preferred);
    return path() != before && !path().isEmpty();
  }
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
  if (m_selection) {
    refreshSelection(m_paths.at(index));
    return !m_paths.isEmpty();
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
  const QString version = FileVersion::key(neighbour);
  // A picture larger than the screen is shown, and preloaded, as its copy.
  const auto known = m_neighbourPictures.constFind(neighbour);
  if (known != m_neighbourPictures.constEnd() && known->version == version) {
    const ImageBudget::Plan plan = ImageBudget::plan(known->estimate, ImageBudget::reducedEdge());
    if (plan.reduced && plan.error.isEmpty()) {
      return ImageBudget::reducedUrl(neighbour, version, copyEdge(plan));
    }
    if (!known->estimate.direct) return ImageBudget::pinnedUrl(neighbour, version, known->estimate);
  }
  return ::imageUrl(neighbour, version);
}

QString ViewerSession::thumbnailUrl(int index, qreal devicePixelRatio) const {
  if (index < 0 || index >= count()) {
    return {};
  }
  const QString& file = m_paths.at(index);
  // "image://thumbs/<ratio>[@<seek%>]~<stamp><encoded path>", as the grid
  // asks for it, so a file already seen there is a disk-cache hit.
  return QStringLiteral("image://thumbs/%1%2~%3%4")
      .arg(devicePixelRatio > 0 ? devicePixelRatio : 1.0)
      .arg(isVideoAt(index) ? QStringLiteral("@40") : QString())
      .arg(ThumbnailSource::version(file, FileVersion::thumbnailKey(file)))
      .arg(QString::fromUtf8(QUrl::toPercentEncoding(file)));
}

bool ViewerSession::isVideoAt(int index) const {
  return index >= 0 && index < count() &&
         CaptureScanner::isVideo(CaptureScanner::mediaSuffix(m_paths.at(index)));
}

QString ViewerSession::fileNameAt(int index) const {
  return index >= 0 && index < count() ? QFileInfo(m_paths.at(index)).fileName() : QString();
}

bool ViewerSession::neighbourIsVideo(int offset) const {
  return CaptureScanner::isVideo(CaptureScanner::mediaSuffix(neighbourPath(offset)));
}

bool ViewerSession::neighbourIsAnimated(int offset) const {
  const QString neighbour = neighbourPath(offset);
  const QString suffix = CaptureScanner::mediaSuffix(neighbour);
  return (suffix == u"gif" || suffix == u"webp") && ImageFormats::looksAnimated(neighbour, suffix) &&
         !DecodeJournal::isBlocked(neighbour);
}

void ViewerSession::forget(const QString& path) {
  m_deletionPaths.remove(path);
  m_explicitEntryPaths.remove(path);
  if (!m_selection && m_folderPaths.contains(path)) {
    ++m_generation; // A listing started before Trash must not put it back.
    const QString current = this->path();
    const QString companion = m_companions.take(path);
    if (!companion.isEmpty()) m_companions.remove(companion);
    m_folderPaths.removeAll(path);
    if (m_folderPaths.isEmpty()) {
      clear();
      emit emptied();
      return;
    }
    rebuildFolderSequence(current == path ? companion : current);
    return;
  }
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
  if (m_selection) watchSelectionFolders();
}

void ViewerSession::renamed(const QString& from, const QString& to, const QString& entry) {
  if (from.isEmpty() || to.isEmpty() || !m_paths.contains(from)) return;
  const QString current = path() == from ? to : path();
  const auto swap = [&](QStringList& list) {
    for (QString& item : list) {
      if (item == from) item = to;
    }
  };
  if (m_opened == from) m_opened = to;
  if (m_startPath == from) m_startPath = to;
  m_explicitEntryPaths.remove(from);
  m_deletionPaths.remove(from);
  if (!entry.isEmpty() && entry != to) {
    (m_selection ? m_explicitEntryPaths : m_deletionPaths).insert(to, entry);
  }
  if (const QString companion = m_companions.take(from); !companion.isEmpty()) {
    m_companions.insert(to, companion);
    m_companions.insert(companion, to);
  }
  QStringList paths = m_paths;
  swap(paths);
  if (m_selection) {
    setSequence(paths, int(paths.indexOf(current)));
    watchSelectionFolders();
    return;
  }
  ++m_generation; // A listing started before the rename names the old file.
  swap(m_folderPaths);
  rebuildFolderSequence(current);
  // The listing puts it in its place in the folder's order.
  startListing();
}

bool ViewerSession::restore(const QString& path, const QString& entry, int index, int serial) {
  if (serial != m_openSerial || m_paths.isEmpty() || path.isEmpty() || m_paths.contains(path) ||
      !QFileInfo(path).isFile()) {
    return false;
  }
  const QString name = entry.isEmpty() ? path : entry;
  if (m_selection) {
    QStringList paths = m_paths;
    const int at = std::clamp(index, 0, int(paths.size()));
    paths.insert(at, path);
    if (name != path) m_explicitEntryPaths.insert(path, name);
    setSequence(paths, at);
    watchSelectionFolders();
    return true;
  }
  if (QFileInfo(name).dir().canonicalPath() != m_folder) return false;
  if (!m_folderPaths.contains(path)) {
    m_folderPaths.insert(std::clamp(index, 0, int(m_folderPaths.size())), path);
  }
  if (name != path) m_deletionPaths.insert(path, name);
  rebuildFolderSequence(path);
  // The listing puts it back in name order with its companion, if any.
  startListing();
  return true;
}

QVariantMap ViewerSession::openDropped(const QList<QUrl>& urls) {
  const FileDrag::Sorted sorted = FileDrag::sort(urls);
  if (sorted.files.isEmpty() && sorted.folders.isEmpty()) {
    return {{QStringLiteral("error"), sorted.skipped.isEmpty()
                                          ? QStringLiteral("Nothing there to open")
                                          : FileDrag::skippedNote(sorted.skipped)}};
  }
  // Each folder opens the library on its own, as `omaroll FOLDER` does; the
  // files beside them still open here.
  QVariantList folders;
  for (const QString& folder : sorted.folders) folders.append(folder);
  QVariantMap result{{QStringLiteral("folders"), folders}};
  QStringList skipped = sorted.skipped;
  if (!sorted.files.isEmpty()) {
    OpenRequest request = OpenRequest::fromInfos(sorted.fileInfos);
    skipped = request.skipped + skipped;
    if (request.files.isEmpty()) {
      if (folders.isEmpty()) {
        return {{QStringLiteral("error"), request.error.isEmpty() ? request.note() : request.error}};
      }
    } else if (!canOpen(request.files)) {
      // A document among them: the library has the document controls.
      result.insert(QStringLiteral("handOff"), true);
      result.insert(QStringLiteral("paths"), sorted.files);
      skipped = sorted.skipped;
    } else {
      open(request.files);
      setDeletionPaths(request.entryPaths);
      result.insert(QStringLiteral("opened"), true);
    }
  }
  result.insert(QStringLiteral("note"), OpenRequest::skippedNote(skipped));
  return result;
}

QString ViewerSession::showSaved(const QString& path, const QString& source, int serial) {
  if (serial != m_openSerial || m_paths.isEmpty()) return {};
  const QString canonical = QFileInfo(path).canonicalFilePath();
  if (canonical.isEmpty()) return {};
  // Still on the original: the copy is shown. Moved on: it is only listed,
  // and the file on screen stays.
  const QString current = this->path();
  const bool show = current == source;
  // The folder watcher may have listed it already.
  const int listed = int(m_paths.indexOf(canonical));
  if (listed >= 0) {
    if (show) jump(listed);
    return show ? QStringLiteral("shown") : QStringLiteral("listed");
  }
  if (m_selection) {
    const int from = int(m_paths.indexOf(source));
    QStringList paths = m_paths;
    const int at = from >= 0 ? from + 1 : int(paths.size());
    paths.insert(at, canonical);
    setSequence(paths, show ? at : int(paths.indexOf(current)));
    watchSelectionFolders();
    return show ? QStringLiteral("shown") : QStringLiteral("listed");
  }
  // A copy beside a link is in the link's folder, which may not be this one.
  if (QFileInfo(canonical).absolutePath() != m_folder) return {};
  // Before the folder's listing lands, the opened file is the whole folder.
  if (m_folderPaths.isEmpty()) m_folderPaths = m_paths;
  const int from = int(m_folderPaths.indexOf(source));
  m_folderPaths.insert(from >= 0 ? from + 1 : int(m_folderPaths.size()), canonical);
  rebuildFolderSequence(show ? canonical : current);
  // The listing puts it back in name order with its companion, if any.
  startListing();
  return show ? QStringLiteral("shown") : QStringLiteral("listed");
}

void ViewerSession::refreshSelection(const QString& preferred) {
  QStringList remaining;
  int next = 0;
  const int preferredIndex = int(m_paths.indexOf(preferred));
  for (int index = 0; index < count(); ++index) {
    const QString& candidate = m_paths.at(index);
    const QFileInfo info(candidate);
    if (info.isFile() && info.isReadable()) {
      if (index < (preferredIndex >= 0 ? preferredIndex : m_index)) ++next;
      remaining.append(candidate);
    } else {
      m_explicitEntryPaths.remove(candidate);
    }
  }
  if (remaining.isEmpty()) {
    clear();
    emit emptied();
    return;
  }
  setSequence(remaining, next);
  watchSelectionFolders();
}

void ViewerSession::watchSelectionFolders() {
  QStringList folders;
  for (const QString& path : m_paths) {
    const QString folder = QFileInfo(path).absolutePath();
    if (!folders.contains(folder)) folders.append(folder);
  }
  const QStringList watched = m_watcher.directories();
  for (const QString& folder : watched) {
    if (!folders.contains(folder)) m_watcher.removePath(folder);
  }
  for (const QString& folder : folders) {
    if (!watched.contains(folder)) m_watcher.addPath(folder);
  }
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

QStringList ViewerSession::siblings(const QString& folder, const QString& keep,
                                   QHash<QString, QString>* entryPaths) {
  const QDir directory(folder);
  const QString kept = QFileInfo(keep).fileName();
  const bool keepHere = !keep.isEmpty() && QFileInfo(keep).absolutePath() == directory.absolutePath();
  QStringList names;
  for (const QString& name :
       directory.entryList(QDir::Files | QDir::Hidden | QDir::Readable, QDir::Unsorted)) {
    if (name.startsWith(QLatin1Char('.')) && !(keepHere && name == kept)) {
      continue;
    }
    if (name != kept && !QFileInfo(name).suffix().isEmpty() &&
        !CaptureScanner::isSupported(QFileInfo(name).suffix()) && !name.endsWith(u".svg.gz", Qt::CaseInsensitive)) continue;
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
      if (entryPaths) entryPaths->insert(path, file.absoluteFilePath());
    }
  }
  return paths;
}

QStringList ViewerSession::siblings(const QString& folder, const QString& keep,
                                   QHash<QString, QString>* entryPaths, const QString& order,
                                   bool reversed) {
  return ViewerSort::sorted(siblings(folder, keep, entryPaths), order, reversed);
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
  const QString beforeThumbnailVersion = m_thumbnailVersion;
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
      m_stamp != beforeStamp || m_contentVersion != beforeVersion ||
      m_thumbnailVersion != beforeThumbnailVersion) {
    emit currentChanged();
  }
  // Relisting may replace indexed files without changing the count.
  ++m_sequenceRevision;
  emit sequenceRevisionChanged();
  emit companionPathChanged();
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
  emit companionPathChanged();
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
  // Screens are read here, on the GUI thread.
  m_preloadEdge = ImageBudget::reducedEdge();
  std::array<QUrl, 2> retained;
  for (size_t i = 0; i < retained.size(); ++i) {
    const QString& neighbour = m_pendingPreloadPaths[i];
    if (neighbour.isEmpty()) continue;
    const QString version = FileVersion::key(neighbour);
    // A renamed picture's approval is its pinned read, sized by its header.
    const auto known = m_neighbourPictures.constFind(neighbour);
    const QUrl pinned = known != m_neighbourPictures.cend() && known->version == version &&
                                !known->estimate.direct
                            ? ImageBudget::pinnedUrl(neighbour, version, known->estimate)
                            : QUrl();
    for (const QUrl& approved : m_preloadUrls) {
      if (approved == ::imageUrl(neighbour, version)
          || approved == ImageBudget::reducedUrl(neighbour, version, m_preloadEdge)
          || (!pinned.isEmpty() && approved == pinned)) {
        retained[i] = approved;
        break;
      }
    }
  }
  // Keep only still-relevant approvals while fresh headers are being probed.
  // The displayed Image has already switched to its own URL.
  setPreloadUrls(retained);
  startPreloadProbe();
}

void ViewerSession::startPreloadProbe() {
  if (m_preloadProbeRunning ||
      (m_pendingPreloadPaths[0].isEmpty() && m_pendingPreloadPaths[1].isEmpty())) return;
  m_preloadProbeRunning = true;
  const auto paths = m_pendingPreloadPaths;
  const quint64 generation = m_preloadGeneration;
  const int edge = m_preloadEdge;
  m_preloadProbe.setFuture(QtConcurrent::run([paths, generation, edge] {
    return probePreloads(paths, generation, edge);
  }));
}

ViewerSession::PreloadResult ViewerSession::probePreloads(const std::array<QString, 2>& paths,
                                                         quint64 generation, int edge) {
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
    PinnedImageReader reader(path);
    if (!reader.canRead() || reader.supportsAnimation()) continue;
    ImageBudget::Estimate estimate = ImageBudget::estimate(reader);
    if (!estimate.valid()) continue;
    estimate.direct = !reader.renamed();
    result.pictures[i] = {path, version, estimate};
    // Too large to show whole: its copy waits until it is stepped onto.
    if (ImageBudget::plan(estimate).reduced) continue;
    // Larger than the screen: the copy the viewer shows, counted at the size
    // its decode allocates.
    const bool reduced = ImageBudget::plan(estimate, edge).reduced;
    QSize size = estimate.stored;
    if (reduced) {
      const QSize copy = ImageBudget::reducedSize(estimate, edge);
      size = ImageBudget::decodedSize(estimate,
                                      estimate.oriented != estimate.stored ? copy.transposed() : copy);
    }
    const QImage::Format format = reader.imageFormat();
    const int depth = QImage::toPixelFormat(format).bitsPerPixel();
    const quint64 bytesPerPixel = depth <= 0 ? 16 : std::max(4, (depth + 7) / 8);
    // Compare before multiplying so oversized or hostile headers cannot wrap.
    const quint64 width = quint64(size.width());
    const quint64 height = quint64(size.height());
    if (width > remaining / bytesPerPixel / height) continue;
    if (FileVersion::key(path) != version) continue;
    remaining -= width * height * bytesPerPixel;
    result.candidates[i] = {path, version, reduced ? edge : 0, estimate};
  }
  return result;
}

void ViewerSession::refreshDetails() {
  m_contentVersion = FileVersion::key(path());
  m_thumbnailVersion = ThumbnailSource::version(path(), FileVersion::thumbnailKey(path()));
  watchCurrentFile();
  m_mediaSuffix = CaptureScanner::mediaSuffix(path());
  // Qt's movie reader has no format to pin and journals nothing, so a GIF or
  // WebP plays only when its contents are one and it never crashed Omaroll.
  // Anything else is read as a still, pinned, which shows a blocked file's
  // error instead of opening it.
  m_animated = (m_mediaSuffix == u"gif" || m_mediaSuffix == u"webp") &&
               ImageFormats::looksAnimated(path(), m_mediaSuffix) &&
               !DecodeJournal::isBlocked(path());
  refreshPicture();
  startPictureCheck();
  const QFileInfo info(path());
  // Another file, or this one replaced: its raw size is read again.
  if (m_rawSizePath != path() || m_rawSizeVersion != m_contentVersion || !isRaw()) {
    const bool had = m_rawSize.isValid();
    m_rawSize = QSize();
    m_rawSizeVersion.clear();
    m_rawSizePath.clear();
    if (had) emit rawSizeChanged();
    if (isRaw() && !m_contentVersion.isEmpty()) {
      m_rawSizeVersion = m_contentVersion;
      m_rawSizePath = path();
      startRawSizeProbe();
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

void ViewerSession::startRawSizeProbe() {
  if (m_rawSizeProbeRunning || m_rawSizePath.isEmpty() || m_rawSizeVersion.isEmpty()) return;
  m_rawSizeProbeRunning = true;
  const QString file = m_rawSizePath;
  const QString version = m_rawSizeVersion;
  m_rawSizeProbe.setFuture(QtConcurrent::run([file, version] {
    return RawSizeResult{file, version, CameraRaw::fullSize(file)};
  }));
}

void ViewerSession::startListing() {
  if (m_folder.isEmpty()) {
    m_listingPending = false;
    return;
  }
  ++m_listingRequest;
  if (m_listingRunning) {
    m_listingPending = true;
    return;
  }
  m_listingPending = false;
  m_listingRunning = true;
  const QString folder = m_folder;
  const QString keep = m_opened;
  const quint64 generation = m_generation;
  const quint64 request = m_listingRequest;
  const QString order = m_sortOrder;
  const bool reversed = m_sortReversed;
  m_listing.setFuture(QtConcurrent::run([folder, keep, generation, request, order, reversed] {
    ListingResult result;
    result.generation = generation;
    result.request = request;
    result.paths = siblings(folder, keep, &result.entryPaths, order, reversed);
    result.companions = RawJpegPairs::find(result.paths);
    return result;
  }));
}

void ViewerSession::applyListing(ListingResult listed) {
  const QString current = path();
  const QString previousCompanion = m_companions.value(current);
  if (listed.paths.isEmpty()) {
    // An unreadable folder still shows the file that was opened. A folder
    // that emptied under the window has nothing left to show.
    if (current.isEmpty() || !QFileInfo::exists(current)) {
      clear();
      emit emptied();
    }
    return;
  }
  QString preferred = current;
  if (!listed.paths.contains(current)) {
    preferred = previousCompanion;
    if (!m_contentVersion.isEmpty()) {
      QString renamed;
      for (const QString& candidate : listed.paths) {
        // The full version includes device, inode, size and modification time.
        // Follow only an unchanged, unique file, never a replacement or an
        // ambiguous hardlink. Otherwise retain the nearest surviving item.
        if (FileVersion::key(candidate) != m_contentVersion) continue;
        if (!renamed.isEmpty()) {
          renamed.clear();
          break;
        }
        renamed = candidate;
      }
      if (!renamed.isEmpty()) {
        preferred = renamed;
        if (m_opened == current) m_opened = renamed;
        m_explicitEntryPaths.remove(current);
      }
    }
  }
  m_folderPaths = std::move(listed.paths);
  m_deletionPaths = std::move(listed.entryPaths);
  m_companions = std::move(listed.companions);
  rebuildFolderSequence(preferred);
}

void ViewerSession::rebuildFolderSequence(const QString& preferred) {
  const QStringList paths = m_pairRawJpeg
                                ? RawJpegPairs::grouped(m_folderPaths, m_companions, preferred)
                                : m_folderPaths;
  const int found = int(paths.indexOf(preferred));
  setSequence(paths, found >= 0 ? found : std::max(m_index, 0));
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
  QStringList next;
  if (!current.isEmpty() && QFileInfo::exists(current)) next.append(current);
  const QString source = ThumbnailSource::path(current);
  if (source != current && QFileInfo::exists(source)) next.append(source);
  if (watched == next) return;
  if (!watched.isEmpty()) m_watcher.removePaths(watched);
  if (!next.isEmpty()) m_watcher.addPaths(next);
}

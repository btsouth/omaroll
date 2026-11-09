#pragma once

#include "sources/ImageBudget.h"
#include "sources/RawJpegPairs.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QFutureWatcher>
#include <QObject>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <array>

// The files the quick viewer steps through, and which one is showing.
//
// One opened file means its folder: every picture and video beside it, in the
// natural name order a file manager shows. Several opened files are an explicit
// selection, kept in the order they were handed over. Nothing here touches the
// library; a file opened from the file manager is shown without scanning a
// single watched root.
//
// The folder is listed on a worker thread. Until it lands the opened file is
// the whole sequence, so the picture is on screen before the directory has
// been read.
class ViewerSession final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString path READ path NOTIFY currentChanged)
  Q_PROPERTY(QString deletionPath READ deletionPath NOTIFY deletionPathChanged)
  Q_PROPERTY(QUrl url READ url NOTIFY currentChanged)
  // Empty while a still's header is read on a worker, so a very large
  // picture is never sent to a full decode it does not need.
  Q_PROPERTY(QUrl imageUrl READ imageUrl NOTIFY pictureChanged)
  // A very large still: imageUrl is a copy the size of the screen, fullSize
  // the picture's own upright size, and detailUrl its full resolution when
  // that can be shown. With areas, the part on screen is loaded from areaUrl()
  // instead. pictureNote says why neither can be; pictureError why not even
  // the copy can be decoded.
  Q_PROPERTY(bool reduced READ reduced NOTIFY pictureChanged)
  Q_PROPERTY(QSize fullSize READ fullSize NOTIFY pictureChanged)
  Q_PROPERTY(QUrl detailUrl READ detailUrl NOTIFY pictureChanged)
  Q_PROPERTY(bool areas READ areas NOTIFY pictureChanged)
  Q_PROPERTY(QString pictureNote READ pictureNote NOTIFY pictureChanged)
  Q_PROPERTY(QString pictureError READ pictureError NOTIFY pictureChanged)
  // A still that shows but is cut off, said beside it. Read on a worker.
  Q_PROPERTY(QString pictureWarning READ pictureWarning NOTIFY pictureCheckChanged)
  // Why a still could not be shown, once diagnosePicture() has looked:
  // {cause, title, detail} from PictureDiagnosis, or empty.
  Q_PROPERTY(QVariantMap pictureFailure READ pictureFailure NOTIFY pictureCheckChanged)
  Q_PROPERTY(QString contentVersion READ contentVersion NOTIFY currentChanged)
  Q_PROPERTY(QString thumbnailVersion READ thumbnailVersion NOTIFY currentChanged)
  Q_PROPERTY(QString fileName READ fileName NOTIFY currentChanged)
  Q_PROPERTY(QString folder READ folder NOTIFY currentChanged)
  Q_PROPERTY(bool isVideo READ isVideo NOTIFY currentChanged)
  Q_PROPERTY(bool isAnimated READ isAnimated NOTIFY currentChanged)
  // A camera raw: url is its embedded preview, rawUrl the full decode and
  // rawSize what that decode measures, which the preview may fall short of.
  Q_PROPERTY(bool isRaw READ isRaw NOTIFY currentChanged)
  Q_PROPERTY(QUrl rawUrl READ rawUrl NOTIFY currentChanged)
  Q_PROPERTY(QSize rawSize READ rawSize NOTIFY rawSizeChanged)
  Q_PROPERTY(double stamp READ stamp NOTIFY currentChanged)
  Q_PROPERTY(QString sizeLabel READ sizeLabel NOTIFY currentChanged)
  Q_PROPERTY(QString dateLabel READ dateLabel NOTIFY currentChanged)
  Q_PROPERTY(int index READ index NOTIFY currentChanged)
  Q_PROPERTY(int count READ count NOTIFY sequenceChanged)
  Q_PROPERTY(quint64 sequenceRevision READ sequenceRevision NOTIFY sequenceRevisionChanged)
  Q_PROPERTY(bool selection READ selection NOTIFY sequenceChanged)
  Q_PROPERTY(bool pairRawJpeg READ pairRawJpeg WRITE setPairRawJpeg NOTIFY pairRawJpegChanged)
  Q_PROPERTY(QString companionPath READ companionPath NOTIFY companionPathChanged)
  Q_PROPERTY(QUrl nextPreloadUrl READ nextPreloadUrl NOTIFY preloadsChanged)
  Q_PROPERTY(QUrl previousPreloadUrl READ previousPreloadUrl NOTIFY preloadsChanged)

public:
  explicit ViewerSession(QObject* parent = nullptr);
  ~ViewerSession() override;

  [[nodiscard]] QString path() const { return m_paths.value(m_index); }
  [[nodiscard]] QString deletionPath() const {
    return m_explicitEntryPaths.value(path(), m_deletionPaths.value(path(), path()));
  }
  [[nodiscard]] QUrl url() const;
  [[nodiscard]] QUrl imageUrl() const;
  [[nodiscard]] bool reduced() const { return m_pictureKnown && m_plan.reduced; }
  [[nodiscard]] QSize fullSize() const { return reduced() ? m_plan.size : QSize(); }
  [[nodiscard]] bool areas() const { return reduced() && m_plan.areas; }
  // |area| of the upright picture, decoded at most |size| large.
  Q_INVOKABLE QUrl areaUrl(const QRect& area, const QSize& size) const;
  [[nodiscard]] QUrl detailUrl() const;
  [[nodiscard]] QString pictureNote() const {
    return m_pictureKnown && !m_plan.areas ? m_plan.note : QString();
  }
  [[nodiscard]] QString pictureError() const { return m_pictureKnown ? m_plan.error : QString(); }
  [[nodiscard]] QString pictureWarning() const { return m_pictureWarning; }
  [[nodiscard]] QVariantMap pictureFailure() const { return m_pictureFailure; }
  // The viewer could not show the current still: find out why, on a worker.
  Q_INVOKABLE void diagnosePicture();
  // The player could not open the current video: the same, for a video.
  Q_INVOKABLE void diagnoseVideo(const QString& playerError);
  [[nodiscard]] QString contentVersion() const { return m_contentVersion; }
  [[nodiscard]] QString thumbnailVersion() const { return m_thumbnailVersion; }
  [[nodiscard]] QString fileName() const;
  [[nodiscard]] QString folder() const;
  [[nodiscard]] bool isVideo() const;
  [[nodiscard]] bool isAnimated() const;
  [[nodiscard]] bool isRaw() const;
  [[nodiscard]] QUrl rawUrl() const;
  [[nodiscard]] QSize rawSize() const { return m_rawSize; }
  [[nodiscard]] double stamp() const { return m_stamp; }
  [[nodiscard]] QString sizeLabel() const { return m_sizeLabel; }
  [[nodiscard]] QString dateLabel() const { return m_dateLabel; }
  [[nodiscard]] int index() const { return m_index; }
  [[nodiscard]] int count() const { return int(m_paths.size()); }
  [[nodiscard]] quint64 sequenceRevision() const { return m_sequenceRevision; }
  [[nodiscard]] bool selection() const { return m_selection; }
  [[nodiscard]] bool pairRawJpeg() const { return m_pairRawJpeg; }
  void setPairRawJpeg(bool value);
  [[nodiscard]] QString companionPath() const;
  Q_INVOKABLE bool switchCompanion();
  // Every file this viewer steps through, in order.
  [[nodiscard]] QStringList sequence() const { return m_paths; }
  [[nodiscard]] QUrl nextPreloadUrl() const { return m_preloadUrls[0]; }
  [[nodiscard]] QUrl previousPreloadUrl() const { return m_preloadUrls[1]; }

  // Paths must already be canonical, as OpenRequest makes them.
  Q_INVOKABLE void open(const QStringList& paths);
  // A start time requested with --start, for one video of this window's
  // request. It is used once, and dropped when the window opens other files.
  void setStartPosition(const QString& path, qint64 milliseconds);
  Q_INVOKABLE [[nodiscard]] qint64 pendingStartPosition(const QString& path) const {
    return path == m_startPath ? m_startMs : -1;
  }
  Q_INVOKABLE qint64 takeStartPosition(const QString& path);
  // Merge new requests without losing aliases for retained selection members.
  void setDeletionPaths(const QHash<QString, QString>& entryPaths);
  // Forget the sequence, so a closed window does not flash the last picture
  // when it is opened onto another one.
  Q_INVOKABLE void clear();

  // Moves through the sequence, wrapping at either end. False when there is
  // nowhere else to go.
  Q_INVOKABLE bool step(int direction);
  Q_INVOKABLE bool jump(int index);

  // The file |offset| steps away, for preloading the next picture.
  Q_INVOKABLE QUrl neighbourUrl(int offset) const;
  Q_INVOKABLE QUrl neighbourImageUrl(int offset) const;
  Q_INVOKABLE bool neighbourIsVideo(int offset) const;
  Q_INVOKABLE bool neighbourIsAnimated(int offset) const;

  // The filmstrip's tile for the file at |index|: the library's thumbnail
  // URL, with the file's mtime so a rewrite is not served from Qt's cache.
  Q_INVOKABLE QString thumbnailUrl(int index, qreal devicePixelRatio) const;
  Q_INVOKABLE bool isVideoAt(int index) const;
  Q_INVOKABLE QString fileNameAt(int index) const;
  Q_INVOKABLE QString rawFormatAt(int index) const;
  Q_INVOKABLE QString companionPathAt(int index) const;

  // A file this window just moved to the Trash. The next one takes its place,
  // or the previous one at the end; an empty sequence emits emptied().
  Q_INVOKABLE void forget(const QString& path);
  // Changes whenever open() or clear() starts another set of files.
  Q_INVOKABLE [[nodiscard]] int openSerial() const { return m_openSerial; }
  // Undo of forget(): a file put back from the Trash returns at |index| and
  // becomes the current one. |entry| is the name it was trashed under when
  // that differs from |path| (a symlink). |serial| is openSerial() when it
  // was trashed; false when this session has moved on to other files, or the
  // file is not there.
  Q_INVOKABLE bool restore(const QString& path, const QString& entry, int index, int serial);

  // Files dropped on this window, opened here the way `omaroll FILE...`
  // opens them: {opened: true, folders, note}, |note| naming anything
  // skipped. Each of |folders| is for the library, one launch each, as
  // `omaroll FOLDER` would be. Files that include a document are {handOff:
  // true, paths} for the library too. {error} when nothing could be opened.
  Q_INVOKABLE QVariantMap openDropped(const QList<QUrl>& urls);
  // A copy this window saved of |source|: it joins the sequence next to
  // |source|, and becomes the current file only if |source| still is.
  // |serial| is openSerial() when the save began. "shown" or "listed", or
  // empty when the copy is not in this window's files: the window has moved
  // on, or the copy is in another folder (beside a link to |source|).
  Q_INVOKABLE QString showSaved(const QString& path, const QString& source, int serial);

  // Hands the current file to the library window.
  Q_INVOKABLE void openInLibrary();

  // True when every file can be shown here. PDFs and folders belong to the
  // library, which has the document controls and the grid.
  [[nodiscard]] static bool canOpen(const QStringList& files);
  [[nodiscard]] static bool isViewable(const QString& path);

  // Pictures and videos in |folder| in natural name order. Dotfiles are left
  // out, except |keep|, which is always listed: it is the file the user opened.
  [[nodiscard]] static QStringList siblings(const QString& folder, const QString& keep = {},
                                           QHash<QString, QString>* entryPaths = nullptr);

  // The viewer's window in logical pixels: most of |available|, landscape,
  // the same for every file, with each picture fitted inside it.
  [[nodiscard]] static QSize preferredWindowSize(const QSize& available);

signals:
  void currentChanged();
  void pictureChanged();
  void pictureCheckChanged();
  void deletionPathChanged();
  void rawSizeChanged();
  void sequenceChanged();
  void sequenceRevisionChanged();
  void preloadsChanged();
  void pairRawJpegChanged();
  void companionPathChanged();
  void emptied();
  void libraryRequested(const QString& path);
  void startPositionChanged();

private:
  [[nodiscard]] QString neighbourPath(int offset) const;
  void setSequence(const QStringList& paths, int index);
  void setIndex(int index);
  void refreshDetails();
  void startListing();
  struct ListingResult {
    quint64 generation = 0;
    quint64 request = 0;
    QStringList paths;
    QHash<QString, QString> entryPaths;
    RawJpegPairs::Companions companions;
  };
  void applyListing(ListingResult listed);
  void rebuildFolderSequence(const QString& preferred);
  void refreshSelection(const QString& preferred);
  void watchSelectionFolders();
  void watchFolder(const QString& folder);
  void watchCurrentFile();
  void refreshPreloads();
  void startPreloadProbe();
  void setPreloadUrls(const std::array<QUrl, 2>& urls);
  void startRawSizeProbe();
  // A still that is shown from its file, as opposed to a video, a camera raw's
  // preview or an animation.
  [[nodiscard]] bool isPlainStill() const;
  // The whole still: its file, or the pinned provider's read of it.
  [[nodiscard]] QUrl stillUrl() const;
  void refreshPicture();
  void startPictureProbe();
  void startPictureCheck();

  struct PreloadCandidate {
    QString path;
    QString version;
    // The copy's longest side, for a picture larger than the screen.
    int edge = 0;
    ImageBudget::Estimate estimate;
  };
  struct PictureResult {
    QString path;
    QString version;
    ImageBudget::Estimate estimate;
  };
  void applyPicture(const PictureResult& result);
  struct PreloadResult {
    quint64 generation = 0;
    std::array<PreloadCandidate, 2> candidates;
    // Every neighbour's header, admitted or not, so stepping onto one does
    // not wait for it to be read again.
    std::array<PictureResult, 2> pictures;
  };
  static PreloadResult probePreloads(const std::array<QString, 2>& paths, quint64 generation,
                                     int edge);

  QStringList m_paths;
  QHash<QString, QString> m_deletionPaths;
  QHash<QString, QString> m_explicitEntryPaths;
  QStringList m_folderPaths;
  RawJpegPairs::Companions m_companions;
  bool m_pairRawJpeg = true;
  quint64 m_sequenceRevision = 0;
  int m_index = -1;
  int m_openSerial = 0;
  bool m_selection = false;
  double m_stamp = 0;
  QString m_sizeLabel;
  QString m_dateLabel;
  QString m_mediaSuffix;
  bool m_animated = false;
  ImageBudget::Estimate m_pictureEstimate;
  QSize m_rawSize;
  QString m_contentVersion;
  QString m_thumbnailVersion;
  // The folder being shown and the opened file, for relisting after a change.
  QString m_folder;
  QString m_startPath;
  qint64 m_startMs = -1;
  QString m_opened;
  // A listing that finishes after another open() describes the wrong folder.
  quint64 m_generation = 0;
  quint64 m_listingRequest = 0;
  bool m_listingRunning = false;
  bool m_listingPending = false;
  QFutureWatcher<ListingResult> m_listing;
  QFileSystemWatcher m_watcher;
  QTimer m_relist;
  // One header probe in flight, with navigation replacing the pending pair.
  // Neither the workers nor their results retain this session.
  quint64 m_preloadGeneration = 0;
  int m_preloadEdge = 0;
  bool m_preloadProbeRunning = false;
  std::array<QString, 2> m_pendingPreloadPaths;
  std::array<QUrl, 2> m_preloadUrls;
  QFutureWatcher<PreloadResult> m_preloadProbe;
  // A raw's size comes from LibRaw opening the file, which can stall on a
  // slow disk, so it is read on a worker for the file it was asked for.
  struct RawSizeResult {
    QString path;
    QString version;
    QSize size;
  };
  QFutureWatcher<RawSizeResult> m_rawSizeProbe;
  bool m_rawSizeProbeRunning = false;
  QString m_rawSizePath;
  QString m_rawSizeVersion;
  // The current still's header, read on a worker before it is shown.
  QFutureWatcher<PictureResult> m_pictureProbe;
  bool m_pictureProbeRunning = false;
  bool m_pictureKnown = false;
  QString m_picturePath;
  QString m_pictureVersion;
  ImageBudget::Plan m_plan;
  int m_reducedEdge = 0;
  QHash<QString, PictureResult> m_neighbourPictures;
  // The current still's cut-off check and, when it fails to show, the
  // diagnosis. Results for another file or version are dropped.
  struct CheckResult {
    QString path;
    QString version;
    QString warning;
    QVariantMap failure;
  };
  QFutureWatcher<CheckResult> m_warningCheck;
  QFutureWatcher<CheckResult> m_failureCheck;
  QString m_checkedPath;
  QString m_checkedVersion;
  QString m_pictureWarning;
  QVariantMap m_pictureFailure;
};

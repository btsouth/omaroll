#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

struct CaptureRecord;

// Everything omaroll remembers between runs.
//
// Deliberately small. The zero-config principle means settings exist to change
// defaults, never to make the app work, so nothing here is required for a first
// run to show a full library.
//
// Marks are keyed by path, with remembered identities for external move
// recovery. Unavailable files keep their organization until the user clears it.
class AppSettings final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString organizationError READ organizationError NOTIFY organizationErrorChanged)
  Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
  // How many files carry a rating, so the Browse sheet can leave the rating
  // row out of a library nobody has rated.
  Q_PROPERTY(int ratedCount READ ratedCount NOTIFY marksChanged)
  Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
  Q_PROPERTY(int kindFilter READ kindFilter WRITE setKindFilter NOTIFY kindFilterChanged)
  Q_PROPERTY(
      bool scanDownloads READ scanDownloads WRITE setScanDownloads NOTIFY scanDownloadsChanged)
  Q_PROPERTY(
      int recursionDepth READ recursionDepth WRITE setRecursionDepth NOTIFY recursionDepthChanged)
  Q_PROPERTY(QStringList libraryFolders READ libraryFolders NOTIFY libraryFoldersChanged)
  Q_PROPERTY(QStringList pinnedFolders READ pinnedFolders NOTIFY pinnedFoldersChanged)
  Q_PROPERTY(bool pairRawJpeg READ pairRawJpeg WRITE setPairRawJpeg NOTIFY pairRawJpegChanged)
  Q_PROPERTY(QString imagePrimaryAction READ imagePrimaryAction WRITE setImagePrimaryAction NOTIFY
                 imagePrimaryActionChanged)
  Q_PROPERTY(QString videoPrimaryAction READ videoPrimaryAction WRITE setVideoPrimaryAction NOTIFY
                 videoPrimaryActionChanged)
  Q_PROPERTY(int thumbnailCacheMb READ thumbnailCacheMb WRITE setThumbnailCacheMb NOTIFY
                 thumbnailCacheMbChanged)
  // Target width of a grid tile in logical pixels. Ctrl+wheel and Ctrl+plus
  // or minus step it; the grid flexes the real width to fill each row.
  Q_PROPERTY(int tileWidth READ tileWidth WRITE setTileWidth NOTIFY tileWidthChanged)
  Q_PROPERTY(bool slideshowVideos READ slideshowVideos WRITE setSlideshowVideos NOTIFY
                 slideshowVideosChanged)
  Q_PROPERTY(bool confirmPermanentDelete READ confirmPermanentDelete WRITE setConfirmPermanentDelete NOTIFY
                 confirmPermanentDeleteChanged)
  // Slideshow timing and order.
  Q_PROPERTY(int slideshowIntervalSeconds READ slideshowIntervalSeconds WRITE
                 setSlideshowIntervalSeconds NOTIFY slideshowIntervalSecondsChanged)
  Q_PROPERTY(bool slideshowShuffle READ slideshowShuffle WRITE setSlideshowShuffle NOTIFY
                 slideshowShuffleChanged)
  // The viewer's strip of neighbouring files, shown with its controls once
  // turned on with B or the menu.
  Q_PROPERTY(bool viewerFilmstrip READ viewerFilmstrip WRITE setViewerFilmstrip NOTIFY
                 viewerFilmstripChanged)
  // Video playback preferences, remembered across files and sessions.
  Q_PROPERTY(qreal videoVolume READ videoVolume WRITE setVideoVolume NOTIFY videoVolumeChanged)
  Q_PROPERTY(bool videoMuted READ videoMuted WRITE setVideoMuted NOTIFY videoMutedChanged)
  Q_PROPERTY(bool rememberPlaybackSpeed READ rememberPlaybackSpeed WRITE setRememberPlaybackSpeed
                 NOTIFY rememberPlaybackSpeedChanged)
  Q_PROPERTY(qreal videoPlaybackRate READ videoPlaybackRate WRITE setVideoPlaybackRate
                 NOTIFY videoPlaybackRateChanged)
  Q_PROPERTY(QStringList albumNames READ albumNames NOTIFY albumsChanged)
  Q_PROPERTY(QStringList tagNames READ tagNames NOTIFY tagsChanged)
  Q_PROPERTY(
      QStringList smartCollectionNames READ smartCollectionNames NOTIFY smartCollectionsChanged)
  Q_PROPERTY(QString previousVisit READ previousVisit CONSTANT)
  Q_PROPERTY(bool undoAvailable READ canUndo NOTIFY undoChanged)

public:
  explicit AppSettings(QObject* parent = nullptr);

  [[nodiscard]] QString organizationError() const { return m_organizationError; }
  [[nodiscard]] bool showHidden() const { return m_showHidden; }
  void setShowHidden(bool value);

  [[nodiscard]] int sortMode() const { return m_sortMode; }
  void setSortMode(int value);

  [[nodiscard]] int kindFilter() const { return m_kindFilter; }
  void setKindFilter(int value);

  [[nodiscard]] bool scanDownloads() const { return m_scanDownloads; }
  void setScanDownloads(bool value);

  [[nodiscard]] int recursionDepth() const { return m_recursionDepth; }
  void setRecursionDepth(int value);

  [[nodiscard]] QStringList libraryFolders() const { return m_libraryFolders; }
  Q_INVOKABLE bool addLibraryFolder(const QUrl& folder);
  Q_INVOKABLE void removeLibraryFolder(const QString& folder);
  // Pins are shortcuts independent of library sources. Unpinning never
  // removes a source, and unavailable entries remain until explicitly removed.
  [[nodiscard]] QStringList pinnedFolders() const { return m_pinnedFolders; }
  Q_INVOKABLE bool pinFolder(const QUrl& folder);
  Q_INVOKABLE bool pinFolderPath(const QString& folder) {
    return pinFolder(QUrl::fromLocalFile(folder));
  }
  Q_INVOKABLE void unpinFolder(const QString& folder);
  Q_INVOKABLE [[nodiscard]] bool isFolderPinned(const QString& folder) const;

  [[nodiscard]] bool pairRawJpeg() const { return m_pairRawJpeg; }
  void setPairRawJpeg(bool value);

  [[nodiscard]] QString imagePrimaryAction() const { return m_imagePrimaryAction; }
  void setImagePrimaryAction(const QString& action);
  [[nodiscard]] QString videoPrimaryAction() const { return m_videoPrimaryAction; }
  void setVideoPrimaryAction(const QString& action);
  [[nodiscard]] int thumbnailCacheMb() const { return m_thumbnailCacheMb; }
  void setThumbnailCacheMb(int megabytes);

  [[nodiscard]] int tileWidth() const { return m_tileWidth; }
  void setTileWidth(int width);
  [[nodiscard]] bool slideshowVideos() const { return m_slideshowVideos; }
  void setSlideshowVideos(bool value);
  [[nodiscard]] bool confirmPermanentDelete() const { return m_confirmPermanentDelete; }
  void setConfirmPermanentDelete(bool value);
  [[nodiscard]] int slideshowIntervalSeconds() const { return m_slideshowIntervalSeconds; }
  void setSlideshowIntervalSeconds(int seconds);
  [[nodiscard]] bool slideshowShuffle() const { return m_slideshowShuffle; }
  void setSlideshowShuffle(bool value);
  [[nodiscard]] bool viewerFilmstrip() const { return m_viewerFilmstrip; }
  void setViewerFilmstrip(bool value);

  // Where a video was last left, in milliseconds. Zero means no saved spot.
  // Entries are pruned so a long-lived library does not grow forever.
  Q_INVOKABLE [[nodiscard]] qint64 videoPosition(const QString& path) const;
  Q_INVOKABLE void setVideoPosition(const QString& path, qint64 milliseconds,
                                     const QString& identity = {});
  Q_INVOKABLE [[nodiscard]] QString videoIdentity(const QString& path) const;
  Q_INVOKABLE void clearVideoPosition(const QString& path);

  [[nodiscard]] qreal videoVolume() const { return m_videoVolume; }
  Q_INVOKABLE void prepareVideoPlayback() const;
  void setVideoVolume(qreal value);
  [[nodiscard]] bool videoMuted() const { return m_videoMuted; }
  void setVideoMuted(bool value);
  [[nodiscard]] bool rememberPlaybackSpeed() const { return m_rememberPlaybackSpeed; }
  void setRememberPlaybackSpeed(bool value);
  [[nodiscard]] qreal videoPlaybackRate() const { return m_videoPlaybackRate; }
  void setVideoPlaybackRate(qreal value);

  [[nodiscard]] QStringList albumNames() const;
  Q_INVOKABLE [[nodiscard]] QStringList albumPaths(const QString& name) const;
  Q_INVOKABLE [[nodiscard]] int albumItemCount(const QString& name) const;
  Q_INVOKABLE [[nodiscard]] int unavailableAlbumItemCount(const QString& name) const;
  Q_INVOKABLE bool createAlbum(const QString& name);
  Q_INVOKABLE void deleteAlbum(const QString& name);
  // Moves an album's membership to a new name. False when the target already
  // exists or the name is unusable; membership is never lost on failure.
  Q_INVOKABLE bool renameAlbum(const QString& oldName, const QString& newName);
  Q_INVOKABLE bool addToAlbum(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeFromAlbum(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeUnavailableFromAlbum(const QString& name);
  void reconcileAlbums(const QList<CaptureRecord>& records);

  // Tags nest with a slash: "Travel/Japan" sits under "Travel". Names come
  // back in tree order, and a parent's paths include every descendant's.
  [[nodiscard]] QStringList tagNames() const;
  Q_INVOKABLE [[nodiscard]] QStringList tagPaths(const QString& name) const;
  Q_INVOKABLE [[nodiscard]] QStringList tagsForPath(const QString& path) const;
  Q_INVOKABLE [[nodiscard]] int tagItemCount(const QString& name) const;
  Q_INVOKABLE bool createTag(const QString& name);
  Q_INVOKABLE void deleteTag(const QString& name);
  // Renames a tag and every tag nested under it, so "Travel/Japan" becomes
  // "Trips/Japan" when "Travel" becomes "Trips". False on a name clash; the
  // existing tree is left untouched.
  Q_INVOKABLE bool renameTag(const QString& oldName, const QString& newName);
  Q_INVOKABLE bool addTag(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeTag(const QString& name, const QStringList& paths);
  void reconcileTags(const QList<CaptureRecord>& records);

  // Repoints favourites, hidden, ratings and captions when their file was
  // moved or renamed outside Omaroll, matching the same inode/device size or
  // content fingerprint that albums and tags already use. Unmatched marks
  // remain remembered even when their files are unavailable.
  void reconcileMarks(const QList<CaptureRecord>& records);

  [[nodiscard]] QStringList smartCollectionNames() const;
  Q_INVOKABLE [[nodiscard]] QVariantMap smartCollection(const QString& name) const;
  Q_INVOKABLE bool saveSmartCollection(const QString& name, const QVariantMap& view);
  Q_INVOKABLE void deleteSmartCollection(const QString& name);

  // A versioned, portable snapshot of everything the user organized: albums,
  // tags, favourites, hidden files, ratings, captions and smart collections.
  // Media files themselves are never read or changed. exportOrganization writes
  // atomically; importOrganization validates the whole file before touching any
  // state, so a bad file leaves the current profile exactly as it was. Both
  // return {ok, message}.
  static constexpr int kOrganizationVersion = 1;
  Q_INVOKABLE [[nodiscard]] QVariantMap exportOrganization(const QString& path) const;
  // Restores by replacing the current organization. Callers should confirm
  // first; the original files on disk are untouched either way.
  Q_INVOKABLE [[nodiscard]] QVariantMap importOrganization(const QString& path);

  [[nodiscard]] QString previousVisit() const { return m_previousVisit; }

  Q_INVOKABLE [[nodiscard]] bool isFavorite(const QString& path) const;
  Q_INVOKABLE [[nodiscard]] bool isHidden(const QString& path) const;

  Q_INVOKABLE void toggleFavorite(const QString& path);
  Q_INVOKABLE void toggleHidden(const QString& path);

  // Undo for the destructive organization actions: favourites, hidden files,
  // ratings and captions. One step per action, capped, cleared by nothing else.
  Q_INVOKABLE [[nodiscard]] bool canUndo() const { return !m_undoStack.isEmpty(); }
  Q_INVOKABLE void undo();

  // Stars, 1 to 5. Zero is unrated and is not stored.
  Q_INVOKABLE [[nodiscard]] int rating(const QString& path) const;
  Q_INVOKABLE void setRating(const QStringList& paths, int rating);
  [[nodiscard]] int ratedCount() const { return static_cast<int>(m_ratings.size()); }
  // A short free-text caption. Searched with filenames and picture text.
  // Empty removes the entry.
  Q_INVOKABLE [[nodiscard]] QString caption(const QString& path) const;
  Q_INVOKABLE void setCaption(const QString& path, const QString& text);

  // Keep user-owned organization attached when an explicit in-app rename
  // changes the path. Discovery itself remains read-only.
  Q_INVOKABLE void relocatePath(const QString& oldPath, const QString& newPath);

  // Bulk marks: one persist and one signal for the whole set.
  Q_INVOKABLE void setFavorite(const QStringList& paths, bool on);
  Q_INVOKABLE void setHidden(const QStringList& paths, bool on);

  // Every path with remembered organization, including unavailable files.
  [[nodiscard]] QStringList markedPaths() const;

signals:
  void showHiddenChanged();
  void sortModeChanged();
  void kindFilterChanged();
  void scanDownloadsChanged();
  void recursionDepthChanged();
  void libraryFoldersChanged();
  void pinnedFoldersChanged();
  void pairRawJpegChanged();
  void imagePrimaryActionChanged();
  void videoPrimaryActionChanged();
  void thumbnailCacheMbChanged();
  void tileWidthChanged();
  void slideshowVideosChanged();
  void confirmPermanentDeleteChanged();
  void slideshowIntervalSecondsChanged();
  void slideshowShuffleChanged();
  void viewerFilmstripChanged();
  void videoVolumeChanged();
  void videoMutedChanged();
  void rememberPlaybackSpeedChanged();
  void videoPlaybackRateChanged();
  void albumsChanged();
  void tagsChanged();
  void smartCollectionsChanged();
  // One signal for "a mark changed", so the model can refresh its flags without
  // caring which one it was.
  void marksChanged();
  void undoChanged();
  void organizationErrorChanged();

private:
  struct AlbumEntry {
    QString path;
    qint64 bytes = -1;
    qint64 modified = 0;
    QByteArray fingerprint;
    quint64 device = 0;
    quint64 inode = 0;
    bool resolved = false;
  };

  void persistMarks();
  void persistAlbums();
  void persistTags();
  void persistSmartCollections();
  void persistMarkIdentities();
  // Snapshots the marks (and their identities) before a destructive change, so
  // undo can put them back exactly.
  void pushMarksUndo();
  void clearMarksUndo();
  bool syncOrganization();
  void setValue(const char* key, const QVariant& value);
  // Records (or forgets) the on-disk identity behind a path's marks, so the
  // mark can be found again after an external move.
  void refreshMarkIdentity(const QString& path);
  [[nodiscard]] bool pathHasMark(const QString& path) const;
  bool reconcileCollectionMap(QMap<QString, QList<AlbumEntry>>& collections,
                              const QList<CaptureRecord>& records);
  [[nodiscard]] static bool relocationAvailable(const AlbumEntry& entry);
  [[nodiscard]] static AlbumEntry identityFor(const QString& path);

  QSettings m_settings;
  QString m_organizationError;
  bool m_deferOrganizationSync = false;
  QSet<QString> m_favorites;
  QSet<QString> m_hidden;
  QHash<QString, int> m_ratings;
  QHash<QString, QString> m_captions;
  // Identity of marked files, keyed by their path, for move recovery. Only
  // paths with at least one mark are kept.
  QHash<QString, AlbumEntry> m_markIdentities;

  struct MarksSnapshot {
    QSet<QString> favorites;
    QSet<QString> hidden;
    QHash<QString, int> ratings;
    QHash<QString, QString> captions;
    QHash<QString, AlbumEntry> identities;
  };
  QList<MarksSnapshot> m_undoStack;
  bool m_undoing = false;

  bool m_showHidden = false;
  int m_sortMode = 0;
  int m_kindFilter = -1;
  bool m_scanDownloads = true;
  int m_recursionDepth = 4;
  QStringList m_libraryFolders;
  QStringList m_pinnedFolders;
  bool m_pairRawJpeg = true;
  QString m_imagePrimaryAction = QStringLiteral("preview");
  QString m_videoPrimaryAction = QStringLiteral("preview");
  int m_thumbnailCacheMb = 256;
  int m_tileWidth = 240;
  bool m_slideshowVideos = false;
  bool m_confirmPermanentDelete = true;
  int m_slideshowIntervalSeconds = 4;
  bool m_slideshowShuffle = false;
  bool m_viewerFilmstrip = false;
  qreal m_videoVolume = 0.8;
  bool m_videoMuted = false;
  bool m_rememberPlaybackSpeed = false;
  qreal m_videoPlaybackRate = 1.0;
  // Resume spots, most recently touched first in m_videoRecency.
  QHash<QString, qint64> m_videoPositions;
  QList<QString> m_videoRecency;
  QMap<QString, QList<AlbumEntry>> m_albums;
  QMap<QString, QList<AlbumEntry>> m_tags;
  QMap<QString, QVariantMap> m_smartCollections;
  QString m_previousVisit;
};

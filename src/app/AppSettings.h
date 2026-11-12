#pragma once

#include <functional>

#include <QDate>
#include <QByteArray>
#include <QHash>
#include "app/ShortcutTable.h"
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <atomic>
#include <memory>

struct CaptureRecord;
class BackupWriter;

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
  Q_PROPERTY(bool canRestoreOrganization READ canRestoreOrganization NOTIFY organizationErrorChanged)
  Q_PROPERTY(QString organizationError READ organizationError NOTIFY organizationErrorChanged)
  // Set once a launch finds organization damaged or clearly incomplete while a
  // recent backup holds more. The library offers Restore once, after its first
  // frame.
  Q_PROPERTY(QString launchWarning READ launchWarning NOTIFY launchWarningChanged)
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
  // Similar pictures: 0 strict, 1 normal, 2 loose.
  Q_PROPERTY(int similarStrictness READ similarStrictness WRITE setSimilarStrictness NOTIFY
                 similarStrictnessChanged)
  // Opt-in: read text from pictures in the background after the library scan.
  Q_PROPERTY(bool backgroundTextIndex READ backgroundTextIndex WRITE setBackgroundTextIndex
                 NOTIFY backgroundTextIndexChanged)
  Q_PROPERTY(bool textIndexScreenshotsOnly READ textIndexScreenshotsOnly WRITE
                 setTextIndexScreenshotsOnly NOTIFY textIndexScreenshotsOnlyChanged)
  Q_PROPERTY(QString imagePrimaryAction READ imagePrimaryAction WRITE setImagePrimaryAction NOTIFY
                 imagePrimaryActionChanged)
  Q_PROPERTY(QString videoPrimaryAction READ videoPrimaryAction WRITE setVideoPrimaryAction NOTIFY
                 videoPrimaryActionChanged)
  Q_PROPERTY(int thumbnailCacheMb READ thumbnailCacheMb WRITE setThumbnailCacheMb NOTIFY
                 thumbnailCacheMbChanged)
  // Target width of a grid tile in logical pixels. Ctrl+wheel and Ctrl+plus
  // or minus step it; the grid flexes the real width to fill each row.
  Q_PROPERTY(int tileWidth READ tileWidth WRITE setTileWidth NOTIFY tileWidthChanged)
  Q_PROPERTY(bool showGridFilenames READ showGridFilenames WRITE setShowGridFilenames NOTIFY
                 showGridFilenamesChanged)
  Q_PROPERTY(bool showFolderSidebar READ showFolderSidebar WRITE setShowFolderSidebar NOTIFY showFolderSidebarChanged)
  Q_PROPERTY(int folderSidebarWidth READ folderSidebarWidth WRITE setFolderSidebarWidth NOTIFY folderSidebarWidthChanged)
  Q_PROPERTY(int previewWidth READ previewWidth WRITE setPreviewWidth NOTIFY previewWidthChanged)
  Q_PROPERTY(int previewHeight READ previewHeight WRITE setPreviewHeight NOTIFY previewHeightChanged)
  Q_PROPERTY(bool slideshowVideos READ slideshowVideos WRITE setSlideshowVideos NOTIFY
                 slideshowVideosChanged)
  Q_PROPERTY(bool confirmPermanentDelete READ confirmPermanentDelete WRITE setConfirmPermanentDelete NOTIFY
                 confirmPermanentDeleteChanged)
  // Delete moves to Trash at once, with Undo, instead of asking first.
  Q_PROPERTY(bool trashWithoutAsking READ trashWithoutAsking WRITE setTrashWithoutAsking NOTIFY
                 trashWithoutAskingChanged)
  // Slideshow timing and order.
  Q_PROPERTY(int slideshowIntervalSeconds READ slideshowIntervalSeconds WRITE
                 setSlideshowIntervalSeconds NOTIFY slideshowIntervalSecondsChanged)
  Q_PROPERTY(bool slideshowShuffle READ slideshowShuffle WRITE setSlideshowShuffle NOTIFY
                 slideshowShuffleChanged)
  // The viewer's strip of neighbouring files, shown with its controls once
  // turned on with B or the menu.
  Q_PROPERTY(bool viewerFilmstrip READ viewerFilmstrip WRITE setViewerFilmstrip NOTIFY
                 viewerFilmstripChanged)
  Q_PROPERTY(bool enlargeSmallPictures READ enlargeSmallPictures WRITE setEnlargeSmallPictures NOTIFY
                 enlargeSmallPicturesChanged)
  // The quick viewer's folder order: "name", "taken", "modified" or "size".
  Q_PROPERTY(QString viewerSort READ viewerSort WRITE setViewerSort NOTIFY viewerSortChanged)
  Q_PROPERTY(bool viewerSortReversed READ viewerSortReversed WRITE setViewerSortReversed NOTIFY
                 viewerSortReversedChanged)
  // A checkerboard behind pictures with transparency, in both viewers.
  Q_PROPERTY(bool transparencyCheckerboard READ transparencyCheckerboard WRITE
                 setTransparencyCheckerboard NOTIFY transparencyCheckerboardChanged)
  // Video playback preferences, remembered across files and sessions.
  Q_PROPERTY(qreal videoVolume READ videoVolume WRITE setVideoVolume NOTIFY videoVolumeChanged)
  Q_PROPERTY(bool videoMuted READ videoMuted WRITE setVideoMuted NOTIFY videoMutedChanged)
  Q_PROPERTY(bool rememberPlaybackSpeed READ rememberPlaybackSpeed WRITE setRememberPlaybackSpeed
                 NOTIFY rememberPlaybackSpeedChanged)
  Q_PROPERTY(qreal videoPlaybackRate READ videoPlaybackRate WRITE setVideoPlaybackRate
                 NOTIFY videoPlaybackRateChanged)
  Q_PROPERTY(QStringList albumNames READ albumNames NOTIFY albumsChanged)
  Q_PROPERTY(QStringList tagNames READ tagNames NOTIFY tagsChanged)
  // Keywords, star ratings and descriptions other photo tools keep in files
  // and .xmp sidecars are shown as tags, stars and captions. Read only: Omaroll
  // never writes them. Off empties that layer and keeps what was hidden.
  Q_PROPERTY(bool readEmbeddedMetadata READ readEmbeddedMetadata WRITE setReadEmbeddedMetadata
                 NOTIFY readEmbeddedMetadataChanged)
  Q_PROPERTY(
      QStringList smartCollectionNames READ smartCollectionNames NOTIFY smartCollectionsChanged)
  Q_PROPERTY(QString previousVisit READ previousVisit CONSTANT)
  Q_PROPERTY(bool undoAvailable READ canUndo NOTIFY undoChanged)
  // The step Ctrl+Z would take back next, or 0. A Trash toast offers Undo only
  // while its own move is still the latest thing that happened.
  Q_PROPERTY(int latestUndoId READ latestUndoId NOTIFY undoChanged)

public:
  Q_PROPERTY(QVariantList shortcutRows READ shortcutRows CONSTANT)
  QVariantList shortcutRows() const { return ShortcutTable::rows(); }
  Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
  Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
  QString appVersion() const { return QStringLiteral(OMAROLL_VERSION); }
  QString qtVersion() const { return QString::fromLatin1(qVersion()); }
  Q_INVOKABLE QString diagnostics(const QString &theme, qreal scale) const;
  // Builds the report on a worker thread, since it may wait on hyprctl, then
  // copies it and emits diagnosticsCopied. A click while one is being built
  // is ignored.
  Q_INVOKABLE void copyDiagnostics(const QString &theme, qreal scale);
  // Optional programs that aren't installed, one per package, each with the
  // feature it enables and its pacman command. Checked on each call.
  Q_INVOKABLE QVariantList missingTools() const;
  // This version's release notes, and where a new issue starts.
  Q_PROPERTY(QString releaseNotesUrl READ releaseNotesUrl CONSTANT)
  Q_PROPERTY(QString newIssueUrl READ newIssueUrl CONSTANT)
  QString releaseNotesUrl() const {
    return QStringLiteral("https://github.com/btsouth/omaroll/releases/tag/v") + appVersion();
  }
  QString newIssueUrl() const {
    return QStringLiteral("https://github.com/btsouth/omaroll/issues/new/choose");
  }
  // The key Omarchy's bindings give its screenshot command.
  Q_PROPERTY(QString screenshotKey READ screenshotKey CONSTANT)
  QString screenshotKey() const;
  // Omaroll's optional Hyprland window rules: "installed", "available" when
  // they can be added, or "unsupported" without an Omarchy Lua config.
  Q_PROPERTY(QString windowRules READ windowRules NOTIFY windowRulesChanged)
  QString windowRules() const;
  Q_INVOKABLE QString installWindowRules();
  Q_INVOKABLE QString removeWindowRules();
  // The rules are offered once outside Settings (the first-run library and
  // --set-default). Dismissing that offer, or installing, ends it.
  Q_PROPERTY(bool windowRulesOfferDismissed READ windowRulesOfferDismissed NOTIFY
                 windowRulesChanged)
  [[nodiscard]] bool windowRulesOfferDismissed() const;
  Q_INVOKABLE void dismissWindowRulesOffer();
  static const char* const kWindowRulesOfferDismissedKey;
  // The same for the command line, which runs without loading the profile.
  // A damaged settings file is never written, as in the app, so it stays
  // there for recovery; ending the offer then returns false.
  [[nodiscard]] static bool windowRulesOfferDismissedOnDisk();
  [[nodiscard]] static bool endWindowRulesOfferOnDisk();
  // Where the rules go. Tests point this at a scratch directory.
  void setHyprDirectory(const QString &directory);
  explicit AppSettings(QObject* parent = nullptr);
  // Waits for a snapshot still being written.
  ~AppSettings() override;

  [[nodiscard]] QString organizationError() const { return m_organizationError; }
  [[nodiscard]] QString launchWarning() const { return m_launchWarning; }
  // Keeps organization that only looked incomplete. The snapshot it was
  // compared with is kept as a before-restore copy first, then today's
  // snapshot is taken from what is here now, so the warning does not return.
  // Empty when kept; otherwise why nothing changed (the fuller backup could
  // not be copied aside, so it is not replaced and the warning stays).
  Q_INVOKABLE QString keepCurrentOrganization();
  // Compares what is here with today's or yesterday's snapshot again, as each
  // launch does after loading. For tests.
  void recheckOrganizationHealth() { checkOrganizationHealth(false); }
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
  [[nodiscard]] int similarStrictness() const { return m_similarStrictness; }
  void setSimilarStrictness(int value);
  [[nodiscard]] bool backgroundTextIndex() const { return m_backgroundTextIndex; }
  void setBackgroundTextIndex(bool value);
  [[nodiscard]] bool textIndexScreenshotsOnly() const { return m_textIndexScreenshotsOnly; }
  void setTextIndexScreenshotsOnly(bool value);

  [[nodiscard]] QString imagePrimaryAction() const { return m_imagePrimaryAction; }
  void setImagePrimaryAction(const QString& action);
  [[nodiscard]] QString videoPrimaryAction() const { return m_videoPrimaryAction; }
  void setVideoPrimaryAction(const QString& action);
  [[nodiscard]] int thumbnailCacheMb() const { return m_thumbnailCacheMb; }
  void setThumbnailCacheMb(int megabytes);

  [[nodiscard]] int tileWidth() const { return m_tileWidth; }
  void setTileWidth(int width);
  [[nodiscard]] bool showGridFilenames() const { return m_showGridFilenames; }
  void setShowGridFilenames(bool value);
  [[nodiscard]] bool showFolderSidebar() const { return m_showFolderSidebar; }
  void setShowFolderSidebar(bool value);
  [[nodiscard]] int folderSidebarWidth() const { return m_folderSidebarWidth; }
  void setFolderSidebarWidth(int width);
  [[nodiscard]] int previewWidth() const { return m_previewWidth; }
  void setPreviewWidth(int width);
  [[nodiscard]] int previewHeight() const { return m_previewHeight; }
  void setPreviewHeight(int height);
  [[nodiscard]] bool slideshowVideos() const { return m_slideshowVideos; }
  void setSlideshowVideos(bool value);
  [[nodiscard]] bool confirmPermanentDelete() const { return m_confirmPermanentDelete; }
  void setConfirmPermanentDelete(bool value);
  [[nodiscard]] bool trashWithoutAsking() const { return m_trashWithoutAsking; }
  void setTrashWithoutAsking(bool value);
  [[nodiscard]] int slideshowIntervalSeconds() const { return m_slideshowIntervalSeconds; }
  void setSlideshowIntervalSeconds(int seconds);
  [[nodiscard]] bool slideshowShuffle() const { return m_slideshowShuffle; }
  void setSlideshowShuffle(bool value);
  [[nodiscard]] bool viewerFilmstrip() const { return m_viewerFilmstrip; }
  void setViewerFilmstrip(bool value);
  [[nodiscard]] bool enlargeSmallPictures() const { return m_enlargeSmallPictures; }
  void setEnlargeSmallPictures(bool value);
  [[nodiscard]] QString viewerSort() const { return m_viewerSort; }
  void setViewerSort(const QString& value);
  [[nodiscard]] bool viewerSortReversed() const { return m_viewerSortReversed; }
  void setViewerSortReversed(bool value);
  [[nodiscard]] bool transparencyCheckerboard() const { return m_transparencyCheckerboard; }
  void setTransparencyCheckerboard(bool value);

  // Where a video was last left, in milliseconds. Zero means no saved spot.
  // Entries are pruned so a long-lived library does not grow forever.
  Q_INVOKABLE [[nodiscard]] qint64 videoPosition(const QString& path) const;
  Q_INVOKABLE void setVideoPosition(const QString& path, qint64 milliseconds,
                                     const QString& identity = {});
  Q_INVOKABLE [[nodiscard]] QString videoIdentity(const QString& path) const;
  Q_INVOKABLE void clearVideoPosition(const QString& path);

  [[nodiscard]] qreal videoVolume() const { return m_videoVolume; }
  Q_INVOKABLE void prepareVideoPlayback() const;
  Q_INVOKABLE QObject* videoPresentation(QObject* sink) const;
  Q_INVOKABLE qint64 videoSeekTarget(double milliseconds, double frameRate) const;
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
  // Saved views that open the album or tag go with it: they would show
  // nothing. Returns how many views were removed.
  Q_INVOKABLE int deleteAlbum(const QString& name);
  // Moves an album's membership to a new name. False when the target already
  // exists or the name is unusable; membership is never lost on failure.
  Q_INVOKABLE bool renameAlbum(const QString& oldName, const QString& newName);
  Q_INVOKABLE bool addToAlbum(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeFromAlbum(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeUnavailableFromAlbum(const QString& name);
  void reconcileAlbums(const QList<CaptureRecord>& records);

  // Tags nest with a slash: "Travel/Japan" sits under "Travel". Names come
  // back in tree order, and a parent's paths include every descendant's.
  //
  // Every reader below returns local tags merged with keywords read from the
  // files (setEmbedded), less those removed here: a removal is kept in
  // Omaroll's own settings, never in the file. Names merge ignoring case, and
  // a local tag's spelling wins. Ratings and captions are the local value if
  // there is one, else nothing if the user cleared the file's value, else the
  // file's.
  [[nodiscard]] QStringList tagNames() const;
  Q_INVOKABLE [[nodiscard]] QStringList tagPaths(const QString& name) const;
  Q_INVOKABLE [[nodiscard]] QStringList tagsForPath(const QString& path) const;
  // Every available file's directly assigned tags or albums, for search. One
  // pass over the collections rather than one per file.
  [[nodiscard]] QHash<QString, QStringList> tagsByPath() const;
  [[nodiscard]] QHash<QString, QStringList> albumsByPath() const;
  Q_INVOKABLE [[nodiscard]] int tagItemCount(const QString& name) const;
  Q_INVOKABLE bool createTag(const QString& name);
  Q_INVOKABLE int deleteTag(const QString& name);
  // Renames a tag and every tag nested under it, so "Travel/Japan" becomes
  // "Trips/Japan" when "Travel" becomes "Trips". False on a name clash; the
  // existing tree is left untouched.
  Q_INVOKABLE bool renameTag(const QString& oldName, const QString& newName);
  Q_INVOKABLE bool addTag(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeTag(const QString& name, const QStringList& paths);
  void reconcileTags(const QList<CaptureRecord>& records);

  [[nodiscard]] bool readEmbeddedMetadata() const { return m_readEmbedded; }
  void setReadEmbeddedMetadata(bool value);
  // What one file and its sidecar hold, and the file's identity when it was
  // read, so hiding a keyword across many files needs no file reads here.
  struct EmbeddedValues {
    QStringList keywords;
    int rating = 0;
    QString caption;
    qint64 bytes = -1;
    qint64 modified = 0;
    quint64 device = 0;
    quint64 inode = 0;
    [[nodiscard]] bool isEmpty() const {
      return keywords.isEmpty() && rating == 0 && caption.isEmpty();
    }
  };
  struct EmbeddedUpdate {
    QString path;
    EmbeddedValues values;
  };
  // Values for library files, from the metadata index. One tagsChanged and one
  // marksChanged at most per batch. Empty values forget the file.
  void setEmbedded(const QList<EmbeddedUpdate>& batch);
  // Forgets values for files no longer in the library.
  void retainEmbedded(const QSet<QString>& livePaths);
  // Values the viewer read for the file it shows. A library file's values are
  // updated; any other file's are kept in a small transient set that only
  // per-file reads use, never Browse, counts or search.
  void noteEmbeddedRead(const QString& path, const QStringList& keywords, int rating,
                        const QString& caption);
  // How many removal rows still wait for their content fingerprint, filled in
  // on a worker after a bulk edit. For tests.
  [[nodiscard]] int pendingFingerprints() const;
  void waitForFingerprints();

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
  [[nodiscard]] bool canRestoreOrganization() const;
  // Imports the newest readable daily or weekly snapshot. A before-restore
  // copy holds a state an import replaced on purpose, so it is used only when
  // no scheduled snapshot reads. The message names the snapshot's date.
  Q_INVOKABLE QVariantMap restoreLastGoodBackup();
  // Writes a snapshot now and waits for it. The app's own snapshots are written
  // on a worker; this is for tests, and the date is explicit for rotation tests.
  bool snapshotOrganization(const QDate& date = QDate::currentDate(), bool beforeRestore = false);
  // Waits for snapshot writes still running on the worker, and applies the
  // launch check if it has finished.
  void flushBackups();
  // How long after the last organization change today's snapshot is taken
  // again. Tests shorten it.
  void setBackupRefreshDelay(int milliseconds) { m_snapshotRefreshDelay = milliseconds; }
  void setBackupRefreshMaxWait(int milliseconds) { m_snapshotMaxWait = milliseconds; }
  // A refresh is waiting for changes to settle. Tests wait it out.
  [[nodiscard]] bool backupRefreshPending() const { return m_snapshotRefresh.isActive(); }
  [[nodiscard]] QString backupDirectory() const;
  // Where the Restore picker opens, so the automatic backups are listed.
  Q_INVOKABLE [[nodiscard]] QUrl backupFolderUrl() const;
  Q_INVOKABLE [[nodiscard]] QVariantMap exportOrganization(const QString& path) const;
  // Restores by replacing the current organization. Callers should confirm
  // first; the original files on disk are untouched either way.
  Q_INVOKABLE [[nodiscard]] QVariantMap importOrganization(const QString& path);

  [[nodiscard]] QString previousVisit() const { return m_previousVisit; }

  Q_INVOKABLE [[nodiscard]] bool isFavorite(const QString& path) const;
  Q_INVOKABLE [[nodiscard]] bool isHidden(const QString& path) const;

  Q_INVOKABLE void toggleFavorite(const QString& path);
  Q_INVOKABLE void toggleHidden(const QString& path);

  // One ordered undo history for the destructive actions: favourites, hidden
  // files, ratings and captions, and moves to Trash. One step per action
  // (a whole batch is one step), capped. undo() takes back the newest step and
  // returns what to tell the user.
  Q_INVOKABLE [[nodiscard]] bool canUndo() const { return !m_undoStack.isEmpty(); }
  [[nodiscard]] int latestUndoId() const {
    return m_undoStack.isEmpty() ? 0 : m_undoStack.constLast().id;
  }
  Q_INVOKABLE QString undo();
  // Records files just moved to Trash as one step, from the records
  // ActionLauncher::trashCaptured returns. |origin| is the viewer session the
  // files left, if any; each record may carry the index it had there. Returns
  // the step's id, or 0 when nothing in |records| can be restored.
  Q_INVOKABLE int pushTrashUndo(const QVariantList& records, QObject* origin = nullptr);

  // Stars, 1 to 5. Zero is unrated and is not stored.
  Q_INVOKABLE [[nodiscard]] int rating(const QString& path) const;
  Q_INVOKABLE void setRating(const QStringList& paths, int rating);
  [[nodiscard]] int ratedCount() const;
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
  void windowRulesChanged();
  void diagnosticsCopied();
  void showHiddenChanged();
  void sortModeChanged();
  void kindFilterChanged();
  void scanDownloadsChanged();
  void recursionDepthChanged();
  void libraryFoldersChanged();
  void pinnedFoldersChanged();
  void pairRawJpegChanged();
  void similarStrictnessChanged();
  void backgroundTextIndexChanged();
  void textIndexScreenshotsOnlyChanged();
  void imagePrimaryActionChanged();
  void videoPrimaryActionChanged();
  void thumbnailCacheMbChanged();
  void tileWidthChanged();
  void showGridFilenamesChanged();
  void showFolderSidebarChanged();
  void folderSidebarWidthChanged();
  void previewWidthChanged();
  void previewHeightChanged();
  void slideshowVideosChanged();
  void confirmPermanentDeleteChanged();
  void trashWithoutAskingChanged();
  void slideshowIntervalSecondsChanged();
  void slideshowShuffleChanged();
  void viewerFilmstripChanged();
  void enlargeSmallPicturesChanged();
  void viewerSortChanged();
  void viewerSortReversedChanged();
  void transparencyCheckerboardChanged();
  void videoVolumeChanged();
  void videoMutedChanged();
  void rememberPlaybackSpeedChanged();
  void videoPlaybackRateChanged();
  void albumsChanged();
  void tagsChanged();
  void readEmbeddedMetadataChanged();
  void smartCollectionsChanged();
  // One signal for "a mark changed", so the model can refresh its flags without
  // caring which one it was.
  void marksChanged();
  void undoChanged();
  // Files an undo put back from the Trash, for the viewer that showed them
  // (|origin|, or null) to take them back at their old places and the library
  // to rescan.
  void trashRestored(const QVariantList& records, QObject* origin);
  void organizationErrorChanged();
  void launchWarningChanged();

private:
  QString m_hyprDirectory;
  mutable QString m_screenshotKey;
  bool m_copyingDiagnostics = false;
#ifdef OMAROLL_TESTING
  friend class OmarollTest;
  static thread_local QStringList* s_fingerprintReadPaths;
#endif
  struct AlbumEntry {
    QString path;
    qint64 bytes = -1;
    qint64 modified = 0;
    QByteArray fingerprint;
    quint64 device = 0;
    quint64 inode = 0;
    bool resolved = false;
    bool operator==(const AlbumEntry& other) const = default;
  };

  void persistMarks();
  void persistAlbums();
  void persistTags();
  // Hidden keywords and cleared ratings or captions from files.
  void persistRemovals();
  void persistSmartCollections();
  // Removes the saved views whose |key| ("album" or "tag") matches.
  int removeViewsNaming(const QString& key, const std::function<bool(const QString&)>& matches);
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
  // Reconciles several collection maps in one pass over the scan. Returns
  // which of them changed, in order.
  QList<bool> reconcileCollectionMaps(const QList<QMap<QString, QList<AlbumEntry>>*>& maps,
                                      const QList<CaptureRecord>& records);
  bool reconcileCollectionMap(QMap<QString, QList<AlbumEntry>>& collections,
                              const QList<CaptureRecord>& records) {
    return reconcileCollectionMaps({&collections}, records).value(0);
  }
  [[nodiscard]] static bool relocationAvailable(const AlbumEntry& entry);
  [[nodiscard]] static AlbumEntry identityFor(const QString& path);

  // Implicitly shared copies of the organization, cheap to take.
  struct OrganizationState {
    QMap<QString, QList<AlbumEntry>> albums;
    QMap<QString, QList<AlbumEntry>> tags;
    QMap<QString, QVariantMap> smartCollections;
    QSet<QString> favorites;
    QSet<QString> hidden;
    QHash<QString, int> ratings;
    QHash<QString, QString> captions;
    QHash<QString, AlbumEntry> markIdentities;
    // Optional backup members, written only when there are rows, so a
    // profile that never hid a keyword exports exactly what 1.18 did.
    QMap<QString, QList<AlbumEntry>> removedEmbeddedTags;
    QMap<QString, QList<AlbumEntry>> clearedEmbedded;
  };
  [[nodiscard]] OrganizationState organizationState() const;
  [[nodiscard]] static QByteArray encodeOrganization(const OrganizationState& state);
  // Today's daily (and this week's weekly) snapshot. Encoded here, written,
  // rotated and synced to disk on the backup thread. Without `replace` it
  // keeps a snapshot already written today; the refresh after changes, the
  // one at exit and the one after an import replace it.
  void scheduleDailySnapshot(bool replace = false);
  // After each saved organization change: today's first snapshot now, and a
  // refresh a few minutes after the last change.
  void noteOrganizationSaved();
  // Compares what loaded with today's or yesterday's snapshot, on the backup
  // thread, before any snapshot of this launch can replace them.
  void checkOrganizationHealth(bool existingProfile);
  void applyOrganizationHealth();
  [[nodiscard]] bool snapshotsHeld() const { return m_healthPending || m_organizationSuspect; }
  [[nodiscard]] static int organizationSize(const OrganizationState& state);
  // The size of what a snapshot file restores, or -1 when an import would
  // refuse it.
  [[nodiscard]] static int snapshotSize(const QByteArray& payload);
  // Before |day|'s snapshot in |path| is written over (|replace|), or first
  // written, with a state of size |replacing| that looks incomplete against
  // it or the newest earlier daily, keeps that snapshot as a before-change
  // copy. False if the copy could not be written.
  static bool keepFullerSnapshot(const QString& path, const QDate& day, int replacing, bool replace);
  // keepFullerSnapshot, then the day's daily and weekly snapshots.
  static bool writeDailySnapshot(const QString& path, const QDate& day, const QByteArray& payload, int size,
                                 bool replace);
  // A backup file's contents, checked the way an import checks them. Pure,
  // so the launch check can run it on the backup thread.
  struct DecodedOrganization {
    QString exportedAt;
    QMap<QString, QList<AlbumEntry>> albums;
    QMap<QString, QList<AlbumEntry>> tags;
    QSet<QString> favorites;
    QSet<QString> hidden;
    QHash<QString, int> ratings;
    QHash<QString, QString> captions;
    QMap<QString, QVariantMap> smartCollections;
    QHash<QString, AlbumEntry> markIdentities;
    QMap<QString, QList<AlbumEntry>> removedEmbeddedTags;
    QMap<QString, QList<AlbumEntry>> clearedEmbedded;
  };
  // Empty when |payload| decodes; otherwise {ok: false, message}.
  [[nodiscard]] static QVariantMap decodeOrganization(const QByteArray& payload, DecodedOrganization& out);
  // Writes a before-restore copy now. Returns false if it could not be saved.
  bool saveBeforeRestore(const OrganizationState& state);
  // Copies the raw settings file before the resume-position migration.
  bool backupLegacySettings();

  QSettings m_settings;
  // Writes snapshot files on its own thread, one after another.
  std::unique_ptr<BackupWriter> m_backupWriter;
  // Set when a damaged file is opened again after a failed repair. QSettings
  // shares its parsed copy of an unchanged file between instances, so the new
  // instance would not report the format error itself.
  bool m_damaged = false;
  [[nodiscard]] QSettings::Status settingsStatus() const {
    return m_damaged ? QSettings::FormatError : m_settings.status();
  }
  QDate m_snapshotDate;
  QTimer m_snapshotRefresh;
  // Picks up the launch check's result once the backup thread has it.
  QTimer m_healthPoll;
  // A change saved since today's snapshot was last encoded.
  bool m_snapshotDirty = false;
  // Set by the backup thread when a scheduled write fails; the change is then
  // owed again and retried by the next refresh or at exit.
  std::shared_ptr<std::atomic<bool>> m_snapshotWriteFailed = std::make_shared<std::atomic<bool>>(false);
  void noteSnapshotFailure();
  // How much organization the profile held when it loaded.
  int m_loadedSize = 0;
  // Steady editing keeps restarting the refresh; it still runs this long
  // after the first change it has not taken.
  QElapsedTimer m_snapshotDirtySince;
  int m_snapshotRefreshDelay = 3 * 60 * 1000;
  int m_snapshotMaxWait = 15 * 60 * 1000;
  void startSnapshotRefresh();
  // Until the launch check reports, and while organization looks incomplete,
  // no snapshot is written, so a damaged load cannot replace a good backup.
  bool m_healthPending = false;
  bool m_organizationSuspect = false;
  bool m_snapshotAfterHealth = false;
  struct HealthReport;
  std::shared_ptr<HealthReport> m_health;
  QString m_suspectSnapshot;
  QString m_launchWarning;
  // An import saves organization several times; snapshot only its result.
  bool m_restoring = false;
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
    QMap<QString, QList<AlbumEntry>> clearedEmbedded;
  };
  struct UndoStep {
    int id = 0;
    // A marks step restores this snapshot; a Trash step restores its records.
    MarksSnapshot marks;
    QVariantList trashed;
    QPointer<QObject> origin;
    [[nodiscard]] bool isTrash() const { return !trashed.isEmpty(); }
  };
  void pushUndoStep(UndoStep step);
  QList<UndoStep> m_undoStack;
  int m_nextUndoId = 1;
  bool m_undoing = false;

  bool m_showHidden = false;
  int m_sortMode = 0;
  int m_kindFilter = -1;
  bool m_scanDownloads = true;
  int m_recursionDepth = 4;
  QStringList m_libraryFolders;
  QStringList m_pinnedFolders;
  bool m_pairRawJpeg = true;
  int m_similarStrictness = 1;
  bool m_backgroundTextIndex = false;
  bool m_textIndexScreenshotsOnly = false;
  QString m_imagePrimaryAction = QStringLiteral("preview");
  QString m_videoPrimaryAction = QStringLiteral("preview");
  int m_thumbnailCacheMb = 256;
  int m_tileWidth = 240;
  bool m_showGridFilenames = false;
  bool m_showFolderSidebar = false;
  int m_folderSidebarWidth = 240;
  int m_previewWidth = 1000;
  int m_previewHeight = 700;
  bool m_slideshowVideos = false;
  bool m_confirmPermanentDelete = true;
  int m_slideshowIntervalSeconds = 4;
  bool m_slideshowShuffle = false;
  bool m_viewerFilmstrip = false;
  bool m_enlargeSmallPictures = true;
  QString m_viewerSort = QStringLiteral("name");
  bool m_viewerSortReversed = false;
  bool m_transparencyCheckerboard = true;
  bool m_trashWithoutAsking = false;
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

  // The layer read from files. Never persisted, backed up or counted by the
  // health check; the metadata index reads it again.
  bool m_readEmbedded = true;
  QHash<QString, EmbeddedValues> m_embedded;
  QSet<QString> m_embeddedLive;
  QHash<QString, EmbeddedValues> m_transientEmbedded;
  QStringList m_transientOrder;
  // Per file, its keywords less those hidden here; and each such keyword,
  // case-folded, to the files that carry it, so tagPaths and counts stay
  // cheap. The first spelling seen is kept for keywords no local tag names.
  QHash<QString, QStringList> m_embeddedEffective;
  QHash<QString, QSet<QString>> m_embeddedTagIndex;
  QHash<QString, QString> m_embeddedSpelling;
  // Removal rows, the same shape as tags: a hidden keyword's name (which
  // also hides everything nested under it) to the files it is hidden on;
  // and "rating" or "caption" to the files whose value from the file was
  // cleared. Indexed by path below, from resolved rows only.
  QMap<QString, QList<AlbumEntry>> m_removedEmbeddedTags;
  QMap<QString, QList<AlbumEntry>> m_clearedEmbedded;
  QHash<QString, QStringList> m_removedByPath;
  QSet<QString> m_clearedRatings;
  QSet<QString> m_clearedCaptions;
  // Fills in the content fingerprint of rows written from the scanner's
  // identity, on a worker, a batch at a time.
  // The worker answers with {identity, fingerprint} pairs.
  QFutureWatcher<QList<std::pair<QString, QString>>>* m_fingerprintWatcher = nullptr;
  // What the worker read, by path and identity: the fingerprint, or empty
  // when the file had changed. A row written again for the same identity, or
  // put back by undo, takes it without another read, and a file that changed
  // is not read again on every pass.
  QHash<QString, QString> m_fingerprints;

  [[nodiscard]] const EmbeddedValues* embeddedFor(const QString& path) const;
  [[nodiscard]] QStringList effectiveKeywords(const QString& path, const QStringList& keywords) const;
  void indexEmbedded(const QString& path);
  void rebuildRemovalIndex();
  void rebuildEmbeddedIndex();
  // The local spelling of a keyword, level by level, where a local tag
  // exists; otherwise the file's own.
  [[nodiscard]] QString displayTag(const QString& folded,
                                   const QHash<QString, QString>& localByFold) const;
  [[nodiscard]] QHash<QString, QString> localTagsByFold() const;
  // Files whose own keywords hold |name| or something nested under it.
  [[nodiscard]] QStringList visibleKeywords(const QString& path) const;
  [[nodiscard]] QStringList embeddedCarriers(const QString& name) const;
  [[nodiscard]] QStringList embeddedKeywordsUnder(const QString& path, const QString& name) const;
  [[nodiscard]] AlbumEntry rowFor(const QString& path) const;
  // Makes every level of a normalized tag a local key, keeping the spelling
  // of levels that exist ignoring case. Returns the leaf's key.
  QString ensureTagKeys(const QString& normalized);
  // Adds removal rows, skipping paths that already have one for the name
  // (ignoring case). True when it added any.
  static bool addRows(QMap<QString, QList<AlbumEntry>>& rows, const QString& name,
                      const QList<AlbumEntry>& added);
  static bool dropRows(QMap<QString, QList<AlbumEntry>>& rows, const QString& name, const QString& path);
  void scheduleFingerprints();
  [[nodiscard]] int embeddedRating(const QString& path) const;
  [[nodiscard]] QString embeddedCaption(const QString& path) const;
};

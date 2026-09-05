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
// Favourites, ratings and hidden entries are keyed by absolute path. A file
// that moves loses its mark, which is the honest behaviour for a library that never
// modifies or tracks the files it reads.
class AppSettings final : public QObject {
  Q_OBJECT
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
  Q_PROPERTY(QStringList albumNames READ albumNames NOTIFY albumsChanged)
  Q_PROPERTY(QStringList tagNames READ tagNames NOTIFY tagsChanged)
  Q_PROPERTY(
      QStringList smartCollectionNames READ smartCollectionNames NOTIFY smartCollectionsChanged)
  Q_PROPERTY(QString previousVisit READ previousVisit CONSTANT)

public:
  explicit AppSettings(QObject* parent = nullptr);

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

  [[nodiscard]] QStringList albumNames() const;
  Q_INVOKABLE [[nodiscard]] QStringList albumPaths(const QString& name) const;
  Q_INVOKABLE [[nodiscard]] int albumItemCount(const QString& name) const;
  Q_INVOKABLE [[nodiscard]] int unavailableAlbumItemCount(const QString& name) const;
  Q_INVOKABLE bool createAlbum(const QString& name);
  Q_INVOKABLE void deleteAlbum(const QString& name);
  Q_INVOKABLE bool addToAlbum(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeFromAlbum(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeUnavailableFromAlbum(const QString& name);
  void reconcileAlbums(const QList<CaptureRecord>& records);

  [[nodiscard]] QStringList tagNames() const;
  Q_INVOKABLE [[nodiscard]] QStringList tagPaths(const QString& name) const;
  Q_INVOKABLE [[nodiscard]] QStringList tagsForPath(const QString& path) const;
  Q_INVOKABLE [[nodiscard]] int tagItemCount(const QString& name) const;
  Q_INVOKABLE bool createTag(const QString& name);
  Q_INVOKABLE void deleteTag(const QString& name);
  Q_INVOKABLE bool addTag(const QString& name, const QStringList& paths);
  Q_INVOKABLE void removeTag(const QString& name, const QStringList& paths);
  void reconcileTags(const QList<CaptureRecord>& records);

  [[nodiscard]] QStringList smartCollectionNames() const;
  Q_INVOKABLE [[nodiscard]] QVariantMap smartCollection(const QString& name) const;
  Q_INVOKABLE bool saveSmartCollection(const QString& name, const QVariantMap& view);
  Q_INVOKABLE void deleteSmartCollection(const QString& name);
  [[nodiscard]] QString previousVisit() const { return m_previousVisit; }

  Q_INVOKABLE [[nodiscard]] bool isFavorite(const QString& path) const;
  Q_INVOKABLE [[nodiscard]] bool isHidden(const QString& path) const;

  Q_INVOKABLE void toggleFavorite(const QString& path);
  Q_INVOKABLE void toggleHidden(const QString& path);

  // Stars, 1 to 5. Zero is unrated and is not stored.
  Q_INVOKABLE [[nodiscard]] int rating(const QString& path) const;
  Q_INVOKABLE void setRating(const QStringList& paths, int rating);
  [[nodiscard]] int ratedCount() const { return static_cast<int>(m_ratings.size()); }

  // Keep user-owned organization attached when an explicit in-app rename
  // changes the path. Discovery itself remains read-only.
  Q_INVOKABLE void relocatePath(const QString& oldPath, const QString& newPath);

  // Bulk marks: one persist and one signal for the whole set.
  Q_INVOKABLE void setFavorite(const QStringList& paths, bool on);
  Q_INVOKABLE void setHidden(const QStringList& paths, bool on);

  // Every marked path, for the scan worker to check against the disk.
  [[nodiscard]] QStringList markedPaths() const;

  // Drop marks the worker found to be gone, so the lists cannot grow without
  // bound across years of use.
  void forgetMarks(const QStringList& paths);

signals:
  void showHiddenChanged();
  void sortModeChanged();
  void kindFilterChanged();
  void scanDownloadsChanged();
  void recursionDepthChanged();
  void libraryFoldersChanged();
  void imagePrimaryActionChanged();
  void videoPrimaryActionChanged();
  void thumbnailCacheMbChanged();
  void tileWidthChanged();
  void slideshowVideosChanged();
  void albumsChanged();
  void tagsChanged();
  void smartCollectionsChanged();
  // One signal for "a mark changed", so the model can refresh its flags without
  // caring which one it was.
  void marksChanged();

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
  bool reconcileCollectionMap(QMap<QString, QList<AlbumEntry>>& collections,
                              const QList<CaptureRecord>& records);
  [[nodiscard]] static AlbumEntry identityFor(const QString& path);

  QSettings m_settings;
  QSet<QString> m_favorites;
  QSet<QString> m_hidden;
  QHash<QString, int> m_ratings;

  bool m_showHidden = false;
  int m_sortMode = 0;
  int m_kindFilter = -1;
  bool m_scanDownloads = true;
  int m_recursionDepth = 4;
  QStringList m_libraryFolders;
  QString m_imagePrimaryAction = QStringLiteral("matte");
  QString m_videoPrimaryAction = QStringLiteral("trim");
  int m_thumbnailCacheMb = 256;
  int m_tileWidth = 240;
  bool m_slideshowVideos = false;
  QMap<QString, QList<AlbumEntry>> m_albums;
  QMap<QString, QList<AlbumEntry>> m_tags;
  QMap<QString, QVariantMap> m_smartCollections;
  QString m_previousVisit;
};

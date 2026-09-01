#pragma once

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QUrl>

struct CaptureRecord;

// Everything omaroll remembers between runs.
//
// Deliberately small. The zero-config principle means settings exist to change
// defaults, never to make the app work, so nothing here is required for a first
// run to show a full library.
//
// Favourites and hidden entries are keyed by absolute path. A file that moves
// loses its mark, which is the honest behaviour for a library that never
// modifies or tracks the files it reads.
class AppSettings final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
  Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
  Q_PROPERTY(int kindFilter READ kindFilter WRITE setKindFilter NOTIFY kindFilterChanged)
  Q_PROPERTY(bool scanDownloads READ scanDownloads WRITE setScanDownloads NOTIFY scanDownloadsChanged)
  Q_PROPERTY(int recursionDepth READ recursionDepth WRITE setRecursionDepth NOTIFY recursionDepthChanged)
  Q_PROPERTY(QStringList libraryFolders READ libraryFolders NOTIFY libraryFoldersChanged)
  Q_PROPERTY(QString imagePrimaryAction READ imagePrimaryAction WRITE setImagePrimaryAction
                 NOTIFY imagePrimaryActionChanged)
  Q_PROPERTY(QString videoPrimaryAction READ videoPrimaryAction WRITE setVideoPrimaryAction
                 NOTIFY videoPrimaryActionChanged)
  Q_PROPERTY(int thumbnailCacheMb READ thumbnailCacheMb WRITE setThumbnailCacheMb
                 NOTIFY thumbnailCacheMbChanged)
  Q_PROPERTY(bool slideshowVideos READ slideshowVideos WRITE setSlideshowVideos
                 NOTIFY slideshowVideosChanged)
  Q_PROPERTY(QStringList albumNames READ albumNames NOTIFY albumsChanged)

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

  Q_INVOKABLE [[nodiscard]] bool isFavorite(const QString& path) const;
  Q_INVOKABLE [[nodiscard]] bool isHidden(const QString& path) const;

  Q_INVOKABLE void toggleFavorite(const QString& path);
  Q_INVOKABLE void toggleHidden(const QString& path);

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
  void slideshowVideosChanged();
  void albumsChanged();
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
  [[nodiscard]] static AlbumEntry identityFor(const QString& path);

  QSettings m_settings;
  QSet<QString> m_favorites;
  QSet<QString> m_hidden;

  bool m_showHidden = false;
  int m_sortMode = 0;
  int m_kindFilter = -1;
  bool m_scanDownloads = true;
  int m_recursionDepth = 4;
  QStringList m_libraryFolders;
  QString m_imagePrimaryAction = QStringLiteral("matte");
  QString m_videoPrimaryAction = QStringLiteral("trim");
  int m_thumbnailCacheMb = 256;
  bool m_slideshowVideos = false;
  QMap<QString, QList<AlbumEntry>> m_albums;
};

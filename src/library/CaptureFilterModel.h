#pragma once

#include <QSortFilterProxyModel>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimer>

struct CaptureRecord;

// Sorting, kind filtering and search over the library.
//
// Everything the grid shows goes through here, so QML always talks to one
// model and never has to know whether a row is filtered out. Index-taking
// helpers are re-exposed on this side because a proxy row is not a source row,
// and calling the source with a proxy index is a subtle way to act on the wrong
// file.
class CaptureFilterModel final : public QSortFilterProxyModel {
  Q_OBJECT
  Q_PROPERTY(int kindFilter READ kindFilter WRITE setKindFilter NOTIFY kindFilterChanged)
  Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
  Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
  Q_PROPERTY(bool favoritesOnly READ favoritesOnly WRITE setFavoritesOnly NOTIFY favoritesOnlyChanged)
  Q_PROPERTY(QString folderFilter READ folderFilter WRITE setFolderFilter NOTIFY folderFilterChanged)
  Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
  Q_PROPERTY(QString albumFilter READ albumFilter NOTIFY albumFilterChanged)
  Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
  Q_PROPERTY(bool duplicatesOnly READ duplicatesOnly WRITE setDuplicatesOnly NOTIFY duplicatesOnlyChanged)
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(bool empty READ empty NOTIFY countChanged)
  // How many rows the source holds regardless of the current filter, so the
  // UI can say "nothing matches" rather than "nothing exists".
  Q_PROPERTY(int sourceCount READ sourceCount NOTIFY countChanged)

public:
  enum SortMode {
    NewestFirst,
    OldestFirst,
    LargestFirst,
    SmallestFirst,
    NameAscending,
  };
  Q_ENUM(SortMode)

  // -1 shows every kind. Otherwise a CaptureRecord::Kind value.
  static constexpr int kAllKinds = -1;

  explicit CaptureFilterModel(QObject* parent = nullptr);
  void setSourceModel(QAbstractItemModel* sourceModel) override;

  [[nodiscard]] int kindFilter() const { return m_kindFilter; }
  void setKindFilter(int kind);

  [[nodiscard]] int sortMode() const { return m_sortMode; }
  void setSortMode(int mode);

  [[nodiscard]] QString searchText() const { return m_searchText; }
  void setSearchText(const QString& text);

  [[nodiscard]] bool favoritesOnly() const { return m_favoritesOnly; }
  void setFavoritesOnly(bool value);

  [[nodiscard]] QString folderFilter() const { return m_folderFilter; }
  void setFolderFilter(const QString& folder);
  [[nodiscard]] QStringList folders() const;
  [[nodiscard]] QString albumFilter() const { return m_albumFilter; }
  Q_INVOKABLE void setAlbumFilter(const QString& name, const QStringList& paths);

  // Hidden entries are excluded by default. This is a "don't show me this
  // again" mark, not a security feature, so revealing them is one toggle away.
  [[nodiscard]] bool showHidden() const { return m_showHidden; }
  void setShowHidden(bool value);

  // Exact duplicates are supplied by DuplicateIndex. This filter remains
  // orthogonal to kind, favourites and search, so those controls can narrow a
  // review without hashing the library again.
  [[nodiscard]] bool duplicatesOnly() const { return m_duplicatesOnly; }
  void setDuplicatesOnly(bool value);
  void setDuplicateGroups(const QHash<QString, QString>& groups);

  [[nodiscard]] int count() const { return rowCount(); }
  [[nodiscard]] bool empty() const { return rowCount() == 0; }

  // Row accessors for QML. A proxy row is not a source row, so these exist on
  // this side rather than letting callers reach past the proxy with an index
  // that means something else there.
  Q_INVOKABLE QString pathAt(int row) const;
  Q_INVOKABLE QString fileNameAt(int row) const;
  Q_INVOKABLE QString dayLabelAt(int row) const;
  // The floating grid label is a day in the normal library and a match-set
  // number while reviewing duplicates.
  Q_INVOKABLE QString gridLabelAt(int row) const;
  Q_INVOKABLE QString timeLabelAt(int row) const;
  Q_INVOKABLE QString sizeLabelAt(int row) const;
  Q_INVOKABLE QString kindLabelAt(int row) const;
  Q_INVOKABLE int kindAt(int row) const;
  Q_INVOKABLE bool isVideoAt(int row) const;
  Q_INVOKABLE qint64 stampAt(int row) const;

  // Text found by the local OCR index. Updates are coalesced because a warm
  // cache can deliver many entries in one event-loop turn.
  void setOcrText(const QString& path, const QString& text);

  // The proxy row showing this path, or -1 when it is filtered out or unknown.
  Q_INVOKABLE int rowOf(const QString& path) const;

  // The next visible item from the same directory in the current sort order.
  // Wraps at either end and returns empty when the directory has only one item.
  Q_INVOKABLE QString adjacentPathInFolder(const QString& path, int direction) const;

  // The next item in the visible collection. Used by albums, filtered library
  // views and slideshows; wraps at either end.
  Q_INVOKABLE QString adjacentPath(const QString& path, int direction) const;

  [[nodiscard]] int sourceCount() const;

signals:
  void kindFilterChanged();
  void sortModeChanged();
  void searchTextChanged();
  void favoritesOnlyChanged();
  void folderFilterChanged();
  void foldersChanged();
  void albumFilterChanged();
  void showHiddenChanged();
  void duplicatesOnlyChanged();
  void countChanged();

protected:
  [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                      const QModelIndex& sourceParent) const override;
  [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
  void beginFilterUpdate();
  void endFilterUpdate();
  void rebuildDuplicateOrdinals();
  [[nodiscard]] const CaptureRecord& sourceRecord(int sourceRow) const;
  [[nodiscard]] bool recordLessThan(const CaptureRecord& first,
                                    const CaptureRecord& second) const;

  int m_kindFilter = kAllKinds;
  int m_sortMode = NewestFirst;
  QString m_searchText;
  QStringList m_searchTerms;
  QString m_folderFilter;
  QString m_albumFilter;
  QSet<QString> m_albumPaths;
  bool m_favoritesOnly = false;
  bool m_showHidden = false;
  bool m_duplicatesOnly = false;
  QHash<QString, QString> m_duplicateGroups;
  QHash<QString, int> m_duplicateOrdinals;
  QHash<QString, QString> m_ocrText;
  QTimer m_ocrFilterTimer;
  QList<QMetaObject::Connection> m_sourceConnections;
};

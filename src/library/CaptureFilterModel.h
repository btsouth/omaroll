#pragma once

#include <QDate>
#include <QCollator>
#include <QHash>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

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
  Q_PROPERTY(
      bool favoritesOnly READ favoritesOnly WRITE setFavoritesOnly NOTIFY favoritesOnlyChanged)
  Q_PROPERTY(
      QString folderFilter READ folderFilter WRITE setFolderFilter NOTIFY folderFilterChanged)
  Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
  Q_PROPERTY(QString albumFilter READ albumFilter NOTIFY albumFilterChanged)
  Q_PROPERTY(QString tagFilter READ tagFilter NOTIFY tagFilterChanged)
  Q_PROPERTY(QString dateFrom READ dateFrom WRITE setDateFrom NOTIFY dateRangeChanged)
  Q_PROPERTY(QString dateTo READ dateTo WRITE setDateTo NOTIFY dateRangeChanged)
  Q_PROPERTY(int dateField READ dateField WRITE setDateField NOTIFY dateRangeChanged)
  Q_PROPERTY(QString modifiedAfter READ modifiedAfter NOTIFY dateRangeChanged)
  Q_PROPERTY(QVariantList dateBuckets READ dateBuckets NOTIFY dateBucketsChanged)
  Q_PROPERTY(
      QString smartCollectionFilter READ smartCollectionFilter NOTIFY smartCollectionFilterChanged)
  Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
  Q_PROPERTY(
      bool duplicatesOnly READ duplicatesOnly WRITE setDuplicatesOnly NOTIFY duplicatesOnlyChanged)
  Q_PROPERTY(bool similarOnly READ similarOnly WRITE setSimilarOnly NOTIFY similarOnlyChanged)
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
  Q_INVOKABLE int folderItemCount(const QString& folder) const;
  [[nodiscard]] QString albumFilter() const { return m_albumFilter; }
  Q_INVOKABLE void setAlbumFilter(const QString& name, const QStringList& paths);
  [[nodiscard]] QString tagFilter() const { return m_tagFilter; }
  Q_INVOKABLE void setTagFilter(const QString& name, const QStringList& paths);

  [[nodiscard]] QString dateFrom() const { return m_dateFrom.toString(Qt::ISODate); }
  [[nodiscard]] QString dateTo() const { return m_dateTo.toString(Qt::ISODate); }
  void setDateFrom(const QString& value);
  void setDateTo(const QString& value);
  [[nodiscard]] int dateField() const { return m_dateField; }
  void setDateField(int value);
  Q_INVOKABLE void setDateRange(const QString& from, const QString& to, int field = 0);
  [[nodiscard]] QString modifiedAfter() const { return m_modifiedAfter; }
  Q_INVOKABLE void setModifiedAfter(const QString& value);
  Q_INVOKABLE void clearDateRange();
  [[nodiscard]] QVariantList dateBuckets() const { return m_dateBuckets; }
  Q_INVOKABLE QVariantList dateDays(const QString& monthKey) const;

  [[nodiscard]] QString smartCollectionFilter() const { return m_smartCollectionFilter; }
  Q_INVOKABLE QVariantMap currentView() const;
  Q_INVOKABLE void applyView(const QString& name, const QVariantMap& view,
                             const QStringList& tagPaths = {});
  Q_INVOKABLE void clearSmartCollection();

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
  [[nodiscard]] bool similarOnly() const { return m_similarOnly; }
  void setSimilarOnly(bool value);
  void setSimilarGroups(const QHash<QString, QString>& groups);

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
  Q_INVOKABLE bool isDocumentAt(int row) const;
  Q_INVOKABLE qint64 stampAt(int row) const;
  Q_INVOKABLE QString ocrSnippetAt(int row) const;

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

  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

signals:
  void kindFilterChanged();
  void sortModeChanged();
  void searchTextChanged();
  void favoritesOnlyChanged();
  void folderFilterChanged();
  void foldersChanged();
  void albumFilterChanged();
  void tagFilterChanged();
  void dateRangeChanged();
  void dateBucketsChanged();
  void smartCollectionFilterChanged();
  void showHiddenChanged();
  void duplicatesOnlyChanged();
  void similarOnlyChanged();
  void countChanged();

protected:
  [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                      const QModelIndex& sourceParent) const override;
  [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
  void beginFilterUpdate();
  void endFilterUpdate();
  [[nodiscard]] bool rebuildFolderIndex();
  [[nodiscard]] bool rebuildDateIndex();
  void rebuildDuplicateOrdinals();
  [[nodiscard]] const CaptureRecord& sourceRecord(int sourceRow) const;
  [[nodiscard]] bool recordLessThan(const CaptureRecord& first, const CaptureRecord& second) const;
  [[nodiscard]] QString ocrSnippet(const CaptureRecord& record) const;

  int m_kindFilter = kAllKinds;
  int m_sortMode = NewestFirst;
  QCollator m_nameCollator;
  QString m_searchText;
  QStringList m_searchTerms;
  QString m_folderFilter;
  QStringList m_folders;
  QHash<QString, int> m_folderItemCounts;
  QString m_albumFilter;
  QSet<QString> m_albumPaths;
  QString m_tagFilter;
  QSet<QString> m_tagPaths;
  QDate m_dateFrom;
  QDate m_dateTo;
  int m_dateField = 0;
  QString m_modifiedAfter;
  qint64 m_modifiedAfterMs = 0;
  QVariantList m_dateBuckets;
  QHash<QString, QVariantList> m_dateDays;
  QString m_smartCollectionFilter;
  bool m_favoritesOnly = false;
  bool m_showHidden = false;
  bool m_duplicatesOnly = false;
  bool m_similarOnly = false;
  QHash<QString, QString> m_duplicateGroups;
  QHash<QString, QString> m_similarGroups;
  QHash<QString, int> m_duplicateOrdinals;
  QHash<QString, QString> m_ocrText;
  QHash<QString, QString> m_ocrFolded;
  QTimer m_folderIndexTimer;
  QTimer m_duplicateOrderTimer;
  QTimer m_ocrFilterTimer;
  QList<QMetaObject::Connection> m_sourceConnections;
};

#pragma once

#include <QDate>
#include <QCollator>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "search/SearchQuery.h"

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <vector>

class AppSettings;
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
      QString cameraFilter READ cameraFilter WRITE setCameraFilter NOTIFY cameraFilterChanged)
  Q_PROPERTY(QString lensFilter READ lensFilter WRITE setLensFilter NOTIFY lensFilterChanged)
  Q_PROPERTY(QVariantList cameras READ cameras NOTIFY camerasChanged)
  Q_PROPERTY(QVariantList lenses READ lenses NOTIFY camerasChanged)
  Q_PROPERTY(
      int minimumRating READ minimumRating WRITE setMinimumRating NOTIFY minimumRatingChanged)
  Q_PROPERTY(
      QString smartCollectionFilter READ smartCollectionFilter NOTIFY smartCollectionFilterChanged)
  Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
  Q_PROPERTY(
      bool duplicatesOnly READ duplicatesOnly WRITE setDuplicatesOnly NOTIFY duplicatesOnlyChanged)
  Q_PROPERTY(bool similarOnly READ similarOnly WRITE setSimilarOnly NOTIFY similarOnlyChanged)
  Q_PROPERTY(bool pairRawJpeg READ pairRawJpeg WRITE setPairRawJpeg NOTIFY pairRawJpegChanged)
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(bool empty READ empty NOTIFY countChanged)
  // How many rows the source holds regardless of the current filter, so the
  // UI can say "nothing matches" rather than "nothing exists".
  Q_PROPERTY(int sourceCount READ sourceCount NOTIFY countChanged)
  // Why nothing is shown, so an empty grid can say "nothing matches" rather
  // than "nothing here". Empty while rows are shown. "library": nothing has
  // been found at all. "duplicates" and "similar": those review views found
  // nothing. "search": the search alone hides everything. "filters": the
  // search together with the kind, favourites, rating, camera and lens
  // choices does. "scope": the folder, album, tag, date range or saved view
  // holds nothing to show.
  Q_PROPERTY(QString emptyReason READ emptyReason NOTIFY countChanged)

public:
  enum SortMode {
    NewestFirst,
    OldestFirst,
    LargestFirst,
    SmallestFirst,
    NameAscending,
    RatingFirst,
  };
  Q_ENUM(SortMode)

  // -1 shows every kind. Otherwise a CaptureRecord::Kind value.
  static constexpr int kAllKinds = -1;

  explicit CaptureFilterModel(QObject* parent = nullptr);
  ~CaptureFilterModel() override;
  void setSourceModel(QAbstractItemModel* sourceModel) override;

  [[nodiscard]] int kindFilter() const { return m_kindFilter; }
  void setKindFilter(int kind);

  [[nodiscard]] int sortMode() const { return m_sortMode; }
  void setSortMode(int mode);

  [[nodiscard]] QString searchText() { commitPendingSearch(); return m_searchText; }
  void setSearchText(const QString& text);
  Q_INVOKABLE void queueSearchText(const QString& text);
  Q_INVOKABLE void commitPendingSearch();
  // The plain words and phrases of the committed search, without field
  // tokens. Text indexing only starts for these.
  [[nodiscard]] QString textSearch() const;
  // Values to offer for the token under the cursor: tag:, album:, in:,
  // camera:, lens:, type:, before: and after:, or a field name for a word
  // that starts one. Rows are {label, detail, insert, start, end, complete};
  // replacing [start, end) with insert (plus a space when complete) accepts one.
  Q_INVOKABLE [[nodiscard]] QVariantList searchSuggestions(const QString& text,
                                                           int cursor) const;

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

  // Camera and lens names as the metadata index resolved them. The choice
  // lists carry {name, count} rows, most used first, and stay empty for a
  // library of screenshots so the Browse sheet can leave them out.
  [[nodiscard]] QString cameraFilter() const { return m_cameraFilter; }
  void setCameraFilter(const QString& camera);
  [[nodiscard]] QString lensFilter() const { return m_lensFilter; }
  void setLensFilter(const QString& lens);
  [[nodiscard]] QVariantList cameras() const { return m_cameras; }
  [[nodiscard]] QVariantList lenses() const { return m_lenses; }
  // Stars a file needs to show. Zero shows everything, including unrated.
  [[nodiscard]] int minimumRating() const { return m_minimumRating; }
  void setMinimumRating(int stars);

  [[nodiscard]] QString smartCollectionFilter() const { return m_smartCollectionFilter; }
  Q_INVOKABLE QVariantMap currentView();
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
  [[nodiscard]] bool pairRawJpeg() const { return m_pairRawJpeg; }
  void setPairRawJpeg(bool value);
  // Explicitly opened files take precedence over the default RAW card. Both
  // members explicitly selected means both physical rows remain visible.
  Q_INVOKABLE void setExplicitPaths(const QStringList& paths);

  [[nodiscard]] int count() const { return rowCount(); }
  [[nodiscard]] bool empty() const { return rowCount() == 0; }

  // Row accessors for QML. A proxy row is not a source row, so these exist on
  // this side rather than letting callers reach past the proxy with an index
  // that means something else there.
  Q_INVOKABLE QString pathAt(int row) const;
  Q_INVOKABLE QString deletionPathAt(int row) const;
  Q_INVOKABLE QString companionPathAt(int row) const;
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
  Q_INVOKABLE bool isAnimatedAt(int row) const;
  Q_INVOKABLE qint64 stampAt(int row) const;
  Q_INVOKABLE QString thumbnailVersionAt(int row) const;
  Q_INVOKABLE QString ocrSnippetAt(int row) const;
  // Every role of a row by its QML name, for views that lay rows out
  // themselves rather than through a delegate model.
  Q_INVOKABLE QVariantMap tileAt(int row) const;
  // While a review filter is on, its sets in view order: each a map of
  // first (row), count and label ("Similar set 2"). Empty otherwise.
  Q_INVOKABLE QVariantList reviewSections() const;

  // Text found by the local OCR index. Updates are coalesced because a warm
  // cache can deliver many entries in one event-loop turn. Pictures use this
  // text and documents use their own, so neither producer's updates touch
  // the other's.
  void setOcrText(const QString& path, const QString& text);

  // Text of PDFs. A library of long documents could hold gigabytes of it, so
  // at most documentTextBudget() bytes stay in memory, least recently used
  // out first. Text that leaves memory stays searchable: once a loader is
  // set, it is read back from the disk cache off the GUI thread and matched
  // against the search there. |persisted| text is in that cache; other text
  // stays in memory. Without a loader nothing leaves memory.
  void setDocumentText(const QString& path, const QString& text, bool persisted = true);
  // Reads one document's text from disk, or returns a null string when it is
  // gone. Called on worker threads.
  using TextLoader = std::function<QString(const QString& path)>;
  void setDocumentTextLoader(TextLoader loader);
  static constexpr qsizetype kDocumentTextBudget = 64 * 1024 * 1024;
  [[nodiscard]] qsizetype documentTextBudget() const { return m_documentTextBudget; }
  void setDocumentTextBudget(qsizetype bytes);
  // Bytes of document text in memory, and how many documents are there or
  // only on disk.
  [[nodiscard]] qsizetype documentTextBytes() const { return m_documentTextBytes; }
  [[nodiscard]] int residentDocumentCount() const {
    return static_cast<int>(m_documentText.size());
  }
  [[nodiscard]] int storedDocumentCount() const {
    return static_cast<int>(m_storedDocuments.size());
  }
  // Whether documents on disk are still being matched against the search.
  [[nodiscard]] bool matchingStoredText() const;

  // The proxy row showing this path, or -1 when it is filtered out or unknown.
  Q_INVOKABLE int rowOf(const QString& path) const;

  // The next visible item from the same directory in the current sort order.
  // Wraps at either end and returns empty when the directory has only one item.
  Q_INVOKABLE QString adjacentPathInFolder(const QString& path, int direction) const;

  // The next item in the visible collection. Used by albums, filtered library
  // views and slideshows; wraps at either end.
  Q_INVOKABLE QString adjacentPath(const QString& path, int direction) const;

  [[nodiscard]] int sourceCount() const;
  [[nodiscard]] QString emptyReason() const;
  // The search, kind, favourites, rating, camera and lens choices, cleared at
  // once. The folder, album, tag and date range are kept. A saved view is
  // left, as with any other change to its filters: what remains is no longer
  // that view.
  Q_INVOKABLE void clearRefinements();
  // Every Browse facet and review filter, and the search too when asked, in
  // one filter pass. Kind, favourites and sort are kept. Leaves a saved view.
  Q_INVOKABLE void clearFacets(bool includeSearch = false);

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
  void cameraFilterChanged();
  void lensFilterChanged();
  void camerasChanged();
  void minimumRatingChanged();
  void smartCollectionFilterChanged();
  void showHiddenChanged();
  void duplicatesOnlyChanged();
  void similarOnlyChanged();
  void pairRawJpegChanged();
  void countChanged();
  // A document's text left memory but the disk cache no longer has it; its
  // producer should read it again.
  void documentTextLost(const QString& path);

protected:
  [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                      const QModelIndex& sourceParent) const override;
  [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
  // Folded search fields of one source row. The raw strings are kept so a
  // row can be checked against its record by pointer: implicitly shared text
  // that has not been replaced is the same text.
  struct SearchKeys {
    QString path;
    QString fileName;
    QString camera;
    QString lens;
    QString caption;
    quint64 organization = 0;
    QString name;
    QString folder;
    QString folderWords;
    QString cameraKey;
    QString lensKey;
    QString captionKey;
    QString tags;
    QString albums;
    bool document = false;
    quint64 text = 0;
    // Folded picture or in-memory PDF text as UTF-8, or empty.
    QByteArray ocr;
  };
  struct ResidentText {
    SearchText::FoldedText text;
    qsizetype bytes = 0;
    bool persisted = true;
    std::list<QString>::iterator use;
  };
  // What a document kept on disk holds for the current search.
  struct StoredMatch {
    // Bit i: the i-th text term of the query is in the document's text.
    quint64 terms = 0;
    QString snippet;
  };
  struct StoredJob {
    QString path;
    QString fileName;
    quint64 serial = 0;
  };
  struct StoredScan {
    quint64 generation = 0;
    QHash<QString, std::pair<quint64, StoredMatch>> matches;
    QList<std::pair<QString, quint64>> lost;
  };

  void beginFilterUpdate();
  void endFilterUpdate();
  void rebuildPairing();
  [[nodiscard]] bool manyRowsChange();
  void applySearchText(const QString& text);
  void reloadOrganization();
  // What emptyReason() may leave out when asking whether anything matches.
  enum Relax { RelaxNothing = 0, RelaxSearch = 1, RelaxRefinements = 2 };
  [[nodiscard]] bool matchesFilters(int sourceRow, const CaptureRecord& record,
                                    int relax = RelaxNothing) const;
  [[nodiscard]] bool anyMatches(int relax) const;
  [[nodiscard]] bool matchesSearch(int sourceRow, const CaptureRecord& record) const;
  [[nodiscard]] const SearchKeys& searchKeys(int sourceRow, const CaptureRecord& record) const;
  struct FolderKeys {
    // Folded, from home: what in: matches.
    QString path;
    // Folded, below the library root: what plain words match.
    QString words;
  };
  [[nodiscard]] const FolderKeys& folderKeys(const QString& path) const;
  void reloadLibraryRoots();
  [[nodiscard]] bool rebuildFolderIndex();
  [[nodiscard]] bool rebuildDateIndex();
  [[nodiscard]] bool rebuildCameraIndex();
  void rebuildDuplicateOrdinals();
  [[nodiscard]] const CaptureRecord& sourceRecord(int sourceRow) const;
  [[nodiscard]] bool recordLessThan(const CaptureRecord& first, const CaptureRecord& second) const;
  [[nodiscard]] QString ocrSnippet(const CaptureRecord& record) const;
  [[nodiscard]] const SearchText::FoldedText* textFor(const CaptureRecord& record) const;
  // Whether a document kept on disk holds the text term at |ordinal|.
  [[nodiscard]] bool storedTextHas(const QString& path, int ordinal) const;
  void dropDocumentText(const QString& path);
  void trimDocumentText();
  void releaseKeyText(const QString& path);
  void resetStoredMatches();
  void startStoredScan();
  void storedScanFinished();
  static StoredScan scanStored(const QList<StoredJob>& jobs, const QList<QByteArray>& terms,
                               const QStringList& snippetTerms, const TextLoader& loader,
                               quint64 generation, const std::atomic_bool& cancel);

  int m_kindFilter = kAllKinds;
  int m_sortMode = NewestFirst;
  QCollator m_nameCollator;
  QString m_searchText;
  QString m_pendingSearchText;
  QTimer m_searchTimer;
  QStringList m_searchTerms;
  SearchQuery m_query;
  mutable std::vector<SearchKeys> m_searchKeys;
  mutable QHash<QString, FolderKeys> m_folderKeys;
  QStringList m_libraryRoots;
  AppSettings* m_settings = nullptr;
  // Folded tag and album names per path, one per line.
  QHash<QString, QString> m_tagText;
  QHash<QString, QString> m_albumText;
  quint64 m_organizationVersion = 1;
  quint64 m_textVersion = 1;
  // While a filter change is applied, each row's verdict from the pass that
  // sized the change, so Qt's own pass does not filter every row again.
  std::vector<char> m_accepted;
  bool m_acceptedValid = false;
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
  QString m_cameraFilter;
  QString m_lensFilter;
  QVariantList m_cameras;
  QVariantList m_lenses;
  int m_minimumRating = 0;
  QString m_smartCollectionFilter;
  bool m_favoritesOnly = false;
  bool m_showHidden = false;
  bool m_duplicatesOnly = false;
  bool m_similarOnly = false;
  bool m_pairRawJpeg = true;
  QSet<QString> m_groupedOutPaths;
  QSet<QString> m_explicitPaths;
  QTimer m_pairingTimer;
  QHash<QString, QString> m_duplicateGroups;
  QHash<QString, QString> m_similarGroups;
  QHash<QString, int> m_duplicateOrdinals;
  QHash<QString, SearchText::FoldedText> m_text;
  // Document text in memory, and its use order, least recent first.
  QHash<QString, ResidentText> m_documentText;
  mutable std::list<QString> m_documentUse;
  qsizetype m_documentTextBytes = 0;
  qsizetype m_documentTextBudget = kDocumentTextBudget;
  TextLoader m_documentTextLoader;
  // Documents whose text is only on disk, each with a serial that changes
  // when its text does, so a scan of older text is not taken as current.
  QHash<QString, quint64> m_storedDocuments;
  quint64 m_storedSerial = 0;
  // The text terms the stored matches answer, the documents not matched
  // against them yet, and those that hold at least one.
  QList<QByteArray> m_storedTerms;
  QStringList m_storedSnippetTerms;
  QSet<QString> m_storedPending;
  QHash<QString, StoredMatch> m_storedMatches;
  QFutureWatcher<StoredScan> m_storedWatcher;
  std::shared_ptr<std::atomic_bool> m_storedCancel = std::make_shared<std::atomic_bool>(false);
  quint64 m_storedGeneration = 0;
  bool m_storedScanning = false;
  QTimer m_storedScanTimer;
  QTimer m_folderIndexTimer;
  bool m_metadataIndexesDirty = false;
  QTimer m_duplicateOrderTimer;
  QTimer m_ocrFilterTimer;
  QTimer m_releaseKeysTimer;
  QList<QMetaObject::Connection> m_sourceConnections;
};

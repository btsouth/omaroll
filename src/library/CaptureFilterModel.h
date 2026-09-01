#pragma once

#include <QSortFilterProxyModel>

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
  Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(bool empty READ empty NOTIFY countChanged)

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

  [[nodiscard]] int kindFilter() const { return m_kindFilter; }
  void setKindFilter(int kind);

  [[nodiscard]] int sortMode() const { return m_sortMode; }
  void setSortMode(int mode);

  [[nodiscard]] QString searchText() const { return m_searchText; }
  void setSearchText(const QString& text);

  [[nodiscard]] bool favoritesOnly() const { return m_favoritesOnly; }
  void setFavoritesOnly(bool value);

  // Hidden entries are excluded by default. This is a "don't show me this
  // again" mark, not a security feature, so revealing them is one toggle away.
  [[nodiscard]] bool showHidden() const { return m_showHidden; }
  void setShowHidden(bool value);

  [[nodiscard]] int count() const { return rowCount(); }
  [[nodiscard]] bool empty() const { return rowCount() == 0; }

  // Row accessors for QML. A proxy row is not a source row, so these exist on
  // this side rather than letting callers reach past the proxy with an index
  // that means something else there.
  Q_INVOKABLE QString pathAt(int row) const;
  Q_INVOKABLE QString fileNameAt(int row) const;
  Q_INVOKABLE QString dayLabelAt(int row) const;
  Q_INVOKABLE QString timeLabelAt(int row) const;
  Q_INVOKABLE QString sizeLabelAt(int row) const;
  Q_INVOKABLE QString kindLabelAt(int row) const;
  Q_INVOKABLE int kindAt(int row) const;
  Q_INVOKABLE bool isVideoAt(int row) const;

  // How many rows the source holds regardless of the current filter, so the
  // UI can say "nothing matches" rather than "nothing exists".
  Q_INVOKABLE int sourceCount() const;

signals:
  void kindFilterChanged();
  void sortModeChanged();
  void searchTextChanged();
  void favoritesOnlyChanged();
  void showHiddenChanged();
  void countChanged();

protected:
  [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                      const QModelIndex& sourceParent) const override;
  [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
  int m_kindFilter = kAllKinds;
  int m_sortMode = NewestFirst;
  QString m_searchText;
  bool m_favoritesOnly = false;
  bool m_showHidden = false;
};

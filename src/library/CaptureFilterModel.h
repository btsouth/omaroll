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

  [[nodiscard]] int count() const { return rowCount(); }
  [[nodiscard]] bool empty() const { return rowCount() == 0; }

  Q_INVOKABLE QString pathAt(int row) const;
  Q_INVOKABLE QString fileNameAt(int row) const;
  Q_INVOKABLE QString dayLabelAt(int row) const;

  // How many rows the source holds regardless of the current filter, so the
  // UI can say "nothing matches" rather than "nothing exists".
  Q_INVOKABLE int sourceCount() const;

signals:
  void kindFilterChanged();
  void sortModeChanged();
  void searchTextChanged();
  void countChanged();

protected:
  [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                      const QModelIndex& sourceParent) const override;
  [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
  int m_kindFilter = kAllKinds;
  int m_sortMode = NewestFirst;
  QString m_searchText;
};

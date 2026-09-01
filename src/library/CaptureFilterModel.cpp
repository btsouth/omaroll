#include "library/CaptureFilterModel.h"

#include "library/CaptureRoles.h"

#include <QDateTime>

CaptureFilterModel::CaptureFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
  setDynamicSortFilter(true);
  // lessThan() reads the roles it needs directly, so no single sort role fits;
  // sorting column 0 just gives the proxy something to order.
  sort(0);

  connect(this, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::modelReset, this, &CaptureFilterModel::countChanged);
}

void CaptureFilterModel::setKindFilter(int kind) {
  if (m_kindFilter == kind) {
    return;
  }
  m_kindFilter = kind;
  invalidateFilter();
  emit kindFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setSortMode(int mode) {
  if (m_sortMode == mode) {
    return;
  }
  m_sortMode = mode;
  invalidate();
  emit sortModeChanged();
}

void CaptureFilterModel::setSearchText(const QString& text) {
  if (m_searchText == text) {
    return;
  }
  m_searchText = text;
  invalidateFilter();
  emit searchTextChanged();
  emit countChanged();
}

QString CaptureFilterModel::pathAt(int row) const {
  return data(index(row, 0), CaptureRoles::PathRole).toString();
}

QString CaptureFilterModel::fileNameAt(int row) const {
  return data(index(row, 0), CaptureRoles::FileNameRole).toString();
}

QString CaptureFilterModel::dayLabelAt(int row) const {
  return data(index(row, 0), CaptureRoles::DayLabelRole).toString();
}

int CaptureFilterModel::sourceCount() const {
  return sourceModel() ? sourceModel()->rowCount() : 0;
}

bool CaptureFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
  const QModelIndex row = sourceModel()->index(sourceRow, 0, sourceParent);

  if (m_kindFilter != kAllKinds &&
      row.data(CaptureRoles::KindRole).toInt() != m_kindFilter) {
    return false;
  }

  if (!m_searchText.isEmpty() &&
      !row.data(CaptureRoles::FileNameRole)
           .toString()
           .contains(m_searchText, Qt::CaseInsensitive)) {
    return false;
  }

  return true;
}

bool CaptureFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  switch (m_sortMode) {
  case OldestFirst:
    return left.data(CaptureRoles::CapturedRole).toDateTime() <
           right.data(CaptureRoles::CapturedRole).toDateTime();
  case LargestFirst:
    return left.data(CaptureRoles::BytesRole).toLongLong() >
           right.data(CaptureRoles::BytesRole).toLongLong();
  case SmallestFirst:
    return left.data(CaptureRoles::BytesRole).toLongLong() <
           right.data(CaptureRoles::BytesRole).toLongLong();
  case NameAscending:
    return QString::compare(left.data(CaptureRoles::FileNameRole).toString(),
                            right.data(CaptureRoles::FileNameRole).toString(),
                            Qt::CaseInsensitive) < 0;
  case NewestFirst:
  default:
    return left.data(CaptureRoles::CapturedRole).toDateTime() >
           right.data(CaptureRoles::CapturedRole).toDateTime();
  }
}

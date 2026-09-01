#include "library/CaptureFilterModel.h"

#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <utility>

CaptureFilterModel::CaptureFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
  setDynamicSortFilter(true);
  // lessThan() reads the roles it needs directly, so no single sort role fits;
  // sorting column 0 just gives the proxy something to order.
  sort(0);

  connect(this, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::modelReset, this, &CaptureFilterModel::countChanged);
}

void CaptureFilterModel::setSourceModel(QAbstractItemModel* model) {
  for (const QMetaObject::Connection& connection : std::as_const(m_sourceConnections)) {
    disconnect(connection);
  }
  m_sourceConnections.clear();

  QSortFilterProxyModel::setSourceModel(model);
  if (!model) {
    return;
  }

  // A source insertion can be completely filtered out, in which case the
  // proxy emits no row signal even though sourceCount changed.
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsInserted, this,
                                     &CaptureFilterModel::foldersChanged));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsRemoved, this,
                                     &CaptureFilterModel::foldersChanged));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::modelReset, this,
                                     &CaptureFilterModel::foldersChanged));
}

void CaptureFilterModel::beginFilterUpdate() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
  beginFilterChange();
#endif
}

void CaptureFilterModel::endFilterUpdate() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  endFilterChange(Direction::Rows);
#else
  invalidateFilter();
#endif
}

void CaptureFilterModel::setKindFilter(int kind) {
  if (m_kindFilter == kind) {
    return;
  }
  beginFilterUpdate();
  m_kindFilter = kind;
  endFilterUpdate();
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
  beginFilterUpdate();
  m_searchText = text;
  endFilterUpdate();
  emit searchTextChanged();
  emit countChanged();
}

void CaptureFilterModel::setFavoritesOnly(bool value) {
  if (m_favoritesOnly == value) {
    return;
  }
  beginFilterUpdate();
  m_favoritesOnly = value;
  endFilterUpdate();
  emit favoritesOnlyChanged();
  emit countChanged();
}

void CaptureFilterModel::setFolderFilter(const QString& folder) {
  const QString normalized = folder.isEmpty() ? QString() : QDir::cleanPath(folder);
  if (m_folderFilter == normalized) {
    return;
  }
  beginFilterUpdate();
  m_folderFilter = normalized;
  endFilterUpdate();
  emit folderFilterChanged();
  emit countChanged();
}

QStringList CaptureFilterModel::folders() const {
  QSet<QString> unique;
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const QString path = sourceModel()
                               ->data(sourceModel()->index(row, 0), CaptureRoles::PathRole)
                               .toString();
      if (!path.isEmpty()) {
        unique.insert(QFileInfo(path).absolutePath());
      }
    }
  }
  QStringList result(unique.begin(), unique.end());
  result.sort(Qt::CaseInsensitive);
  return result;
}

void CaptureFilterModel::setAlbumFilter(const QString& name, const QStringList& paths) {
  const QSet<QString> next(paths.begin(), paths.end());
  if (m_albumFilter == name && m_albumPaths == next) {
    return;
  }
  beginFilterUpdate();
  m_albumFilter = name;
  m_albumPaths = next;
  endFilterUpdate();
  emit albumFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setShowHidden(bool value) {
  if (m_showHidden == value) {
    return;
  }
  beginFilterUpdate();
  m_showHidden = value;
  endFilterUpdate();
  emit showHiddenChanged();
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

QString CaptureFilterModel::timeLabelAt(int row) const {
  return data(index(row, 0), CaptureRoles::TimeLabelRole).toString();
}

QString CaptureFilterModel::sizeLabelAt(int row) const {
  return data(index(row, 0), CaptureRoles::SizeLabelRole).toString();
}

QString CaptureFilterModel::kindLabelAt(int row) const {
  return data(index(row, 0), CaptureRoles::KindLabelRole).toString();
}

int CaptureFilterModel::kindAt(int row) const {
  return data(index(row, 0), CaptureRoles::KindRole).toInt();
}

bool CaptureFilterModel::isVideoAt(int row) const {
  return data(index(row, 0), CaptureRoles::IsVideoRole).toBool();
}

qint64 CaptureFilterModel::stampAt(int row) const {
  return data(index(row, 0), CaptureRoles::StampRole).toLongLong();
}

int CaptureFilterModel::rowOf(const QString& path) const {
  if (path.isEmpty()) {
    return -1;
  }
  const int rows = rowCount();
  for (int row = 0; row < rows; ++row) {
    if (data(index(row, 0), CaptureRoles::PathRole).toString() == path) {
      return row;
    }
  }
  return -1;
}

QString CaptureFilterModel::adjacentPathInFolder(const QString& path, int direction) const {
  const int current = rowOf(path);
  const int rows = rowCount();
  if (current < 0 || rows < 2 || direction == 0) {
    return {};
  }

  const QString directory = QFileInfo(path).absolutePath();
  const int step = direction < 0 ? -1 : 1;
  for (int offset = 1; offset < rows; ++offset) {
    const int row = (current + step * offset + rows) % rows;
    const QString candidate = pathAt(row);
    if (QFileInfo(candidate).absolutePath() == directory) {
      return candidate;
    }
  }
  return {};
}

QString CaptureFilterModel::adjacentPath(const QString& path, int direction) const {
  const int current = rowOf(path);
  const int rows = rowCount();
  if (current < 0 || rows < 2 || direction == 0) {
    return {};
  }
  const int next = (current + (direction < 0 ? -1 : 1) + rows) % rows;
  return pathAt(next);
}

int CaptureFilterModel::sourceCount() const {
  return sourceModel() ? sourceModel()->rowCount() : 0;
}

// The source is always a CaptureModel; reading its records directly keeps a
// full re-sort of a large library from building a QVariant per comparison.
const CaptureRecord& CaptureFilterModel::sourceRecord(int sourceRow) const {
  return static_cast<const CaptureModel*>(sourceModel())->recordAt(sourceRow);
}

bool CaptureFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
  if (sourceParent.isValid()) {
    return false;
  }
  const CaptureRecord& record = sourceRecord(sourceRow);

  if (!m_showHidden && record.hidden) {
    return false;
  }
  if (m_favoritesOnly && !record.favorite) {
    return false;
  }
  if (!m_albumFilter.isEmpty() && !m_albumPaths.contains(record.path)) {
    return false;
  }
  if (!m_folderFilter.isEmpty()) {
    const QString directory = QFileInfo(record.path).absolutePath();
    const QString prefix = m_folderFilter.endsWith(QLatin1Char('/'))
                               ? m_folderFilter
                               : m_folderFilter + QLatin1Char('/');
    if (directory != m_folderFilter &&
        !directory.startsWith(prefix)) {
      return false;
    }
  }
  if (m_kindFilter != kAllKinds && static_cast<int>(record.kind) != m_kindFilter) {
    return false;
  }
  if (!m_searchText.isEmpty() && !record.fileName.contains(m_searchText, Qt::CaseInsensitive)) {
    return false;
  }
  return true;
}

bool CaptureFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  const CaptureRecord& a = sourceRecord(left.row());
  const CaptureRecord& b = sourceRecord(right.row());
  const auto pathOrder = [&] {
    return QString::compare(a.path, b.path, Qt::CaseSensitive) < 0;
  };
  switch (m_sortMode) {
  case OldestFirst:
    return a.captured != b.captured ? a.captured < b.captured : pathOrder();
  case LargestFirst:
    return a.bytes != b.bytes ? a.bytes > b.bytes : pathOrder();
  case SmallestFirst:
    return a.bytes != b.bytes ? a.bytes < b.bytes : pathOrder();
  case NameAscending:
    if (const int names = QString::compare(a.fileName, b.fileName, Qt::CaseInsensitive);
        names != 0) {
      return names < 0;
    }
    return pathOrder();
  case NewestFirst:
  default:
    return a.captured != b.captured ? a.captured > b.captured : pathOrder();
  }
}

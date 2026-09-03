#include "library/CaptureFilterModel.h"

#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

CaptureFilterModel::CaptureFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
  setDynamicSortFilter(true);
  // lessThan() reads the roles it needs directly, so no single sort role fits;
  // sorting column 0 just gives the proxy something to order.
  sort(0);

  connect(this, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::modelReset, this, &CaptureFilterModel::countChanged);

  m_ocrFilterTimer.setSingleShot(true);
  m_ocrFilterTimer.setInterval(60);
  connect(&m_ocrFilterTimer, &QTimer::timeout, this, [this] {
    beginFilterUpdate();
    endFilterUpdate();
    if (rowCount() > 0) {
      emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                       {CaptureRoles::OcrSnippetRole});
    }
    emit countChanged();
  });
}

void CaptureFilterModel::setSourceModel(QAbstractItemModel* model) {
  for (const QMetaObject::Connection& connection : std::as_const(m_sourceConnections)) {
    disconnect(connection);
  }
  m_sourceConnections.clear();

  QSortFilterProxyModel::setSourceModel(model);
  if (!model) {
    m_folders.clear();
    m_folderItemCounts.clear();
    emit foldersChanged();
    return;
  }

  // A source insertion can be completely filtered out, in which case the
  // proxy emits no row signal even though sourceCount changed.
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged));
  const auto rebuildFolders = [this] {
    rebuildFolderIndex();
    emit foldersChanged();
  };
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, rebuildFolders));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, rebuildFolders));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::modelReset, this, rebuildFolders));
  const auto rebuildDuplicateOrder = [this] {
    if (!m_duplicateGroups.isEmpty()) {
      rebuildDuplicateOrdinals();
      invalidate();
    }
  };
  m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsInserted, this,
                                     rebuildDuplicateOrder));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsRemoved, this,
                                     rebuildDuplicateOrder));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::modelReset, this,
                                     rebuildDuplicateOrder));
  m_sourceConnections.append(connect(
      model, &QAbstractItemModel::dataChanged, this,
      [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
        if (roles.isEmpty() || roles.contains(CaptureRoles::PathRole)) {
          rebuildFolderIndex();
          emit foldersChanged();
        }
        if (!m_duplicateGroups.isEmpty() &&
            (roles.isEmpty() || roles.contains(CaptureRoles::PathRole) ||
             roles.contains(CaptureRoles::FileNameRole) ||
             roles.contains(CaptureRoles::CapturedRole) ||
             roles.contains(CaptureRoles::BytesRole))) {
          rebuildDuplicateOrdinals();
          invalidate();
        }
      }));
  rebuildFolderIndex();
  emit foldersChanged();
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
  rebuildDuplicateOrdinals();
  invalidate();
  emit sortModeChanged();
}

void CaptureFilterModel::setSearchText(const QString& text) {
  if (m_searchText == text) {
    return;
  }
  beginFilterUpdate();
  m_searchText = text;
  m_searchTerms = text.toCaseFolded().split(QRegularExpression(QStringLiteral("\\s+")),
                                            Qt::SkipEmptyParts);
  endFilterUpdate();
  if (rowCount() > 0) {
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                     {CaptureRoles::OcrSnippetRole});
  }
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
  return m_folders;
}

int CaptureFilterModel::folderItemCount(const QString& folder) const {
  return m_folderItemCounts.value(QDir::cleanPath(folder));
}

void CaptureFilterModel::rebuildFolderIndex() {
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
  m_folders = QStringList(unique.begin(), unique.end());
  m_folders.sort(Qt::CaseInsensitive);

  m_folderItemCounts.clear();
  m_folderItemCounts.reserve(unique.size());
  if (!sourceModel()) {
    return;
  }
  for (int row = 0; row < sourceModel()->rowCount(); ++row) {
    const QString path = sourceModel()
                             ->data(sourceModel()->index(row, 0), CaptureRoles::PathRole)
                             .toString();
    QString directory = QFileInfo(path).absolutePath();
    while (!directory.isEmpty()) {
      if (unique.contains(directory)) {
        ++m_folderItemCounts[directory];
      }
      const QString parent = QFileInfo(directory).path();
      if (parent == directory || parent == QStringLiteral(".")) {
        break;
      }
      directory = parent;
    }
  }
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

void CaptureFilterModel::setDuplicatesOnly(bool value) {
  if (m_duplicatesOnly == value) {
    return;
  }
  beginFilterUpdate();
  m_duplicatesOnly = value;
  endFilterUpdate();
  invalidate();
  emit duplicatesOnlyChanged();
  emit countChanged();
}

void CaptureFilterModel::setDuplicateGroups(const QHash<QString, QString>& groups) {
  if (m_duplicateGroups == groups) {
    return;
  }

  beginFilterUpdate();
  m_duplicateGroups = groups;
  rebuildDuplicateOrdinals();
  endFilterUpdate();
  invalidate();
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

QString CaptureFilterModel::gridLabelAt(int row) const {
  if (!m_duplicatesOnly) {
    return dayLabelAt(row);
  }
  const QString group = m_duplicateGroups.value(pathAt(row));
  const int ordinal = m_duplicateOrdinals.value(group);
  if (ordinal <= 0) {
    return {};
  }
  return QStringLiteral("Exact match %1 of %2")
      .arg(ordinal)
      .arg(m_duplicateOrdinals.size());
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

QString CaptureFilterModel::ocrSnippetAt(int row) const {
  return data(index(row, 0), CaptureRoles::OcrSnippetRole).toString();
}

QVariant CaptureFilterModel::data(const QModelIndex& proxyIndex, int role) const {
  if (role == CaptureRoles::OcrSnippetRole) {
    if (!proxyIndex.isValid()) {
      return {};
    }
    return ocrSnippet(sourceRecord(mapToSource(proxyIndex).row()));
  }
  return QSortFilterProxyModel::data(proxyIndex, role);
}

QHash<int, QByteArray> CaptureFilterModel::roleNames() const {
  QHash<int, QByteArray> roles = QSortFilterProxyModel::roleNames();
  roles.insert(CaptureRoles::OcrSnippetRole, QByteArrayLiteral("ocrSnippet"));
  return roles;
}

void CaptureFilterModel::setOcrText(const QString& path, const QString& text) {
  if (path.isEmpty()) {
    return;
  }
  if (text.isEmpty()) {
    if (m_ocrText.remove(path) == 0) {
      return;
    }
    m_ocrFolded.remove(path);
  } else {
    if (m_ocrText.value(path) == text) {
      return;
    }
    m_ocrText.insert(path, text);
    m_ocrFolded.insert(path, text.toCaseFolded());
  }
  if (!m_searchTerms.isEmpty()) {
    m_ocrFilterTimer.start();
  }
}

int CaptureFilterModel::rowOf(const QString& path) const {
  if (path.isEmpty() || !sourceModel()) {
    return -1;
  }
  const auto* source = static_cast<const CaptureModel*>(sourceModel());
  const int sourceRow = source->rowOf(path);
  if (sourceRow < 0) {
    return -1;
  }
  const QModelIndex proxyIndex = mapFromSource(source->index(sourceRow, 0));
  return proxyIndex.isValid() ? proxyIndex.row() : -1;
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

QString CaptureFilterModel::ocrSnippet(const CaptureRecord& record) const {
  if (m_searchTerms.isEmpty()) {
    return {};
  }
  const QString text = m_ocrText.value(record.path);
  if (text.isEmpty()) {
    return {};
  }
  const QString name = record.fileName.toCaseFolded();
  const QString folded = text.toCaseFolded();
  QString matchedOnlyInText;
  for (const QString& term : m_searchTerms) {
    if (!name.contains(term) && folded.contains(term)) {
      matchedOnlyInText = term;
      break;
    }
  }
  if (matchedOnlyInText.isEmpty()) {
    return {};
  }

  const qsizetype match = text.indexOf(matchedOnlyInText, 0, Qt::CaseInsensitive);
  const qsizetype start = qMax<qsizetype>(0, match - 44);
  const qsizetype end = qMin<qsizetype>(text.size(), match + matchedOnlyInText.size() + 56);
  QString snippet = text.mid(start, end - start).simplified();
  if (start > 0) {
    snippet.prepend(QChar(0x2026));
  }
  if (end < text.size()) {
    snippet.append(QChar(0x2026));
  }
  return snippet;
}

bool CaptureFilterModel::recordLessThan(const CaptureRecord& first,
                                        const CaptureRecord& second) const {
  const auto pathOrder = [&] {
    return QString::compare(first.path, second.path, Qt::CaseSensitive) < 0;
  };
  switch (m_sortMode) {
  case OldestFirst:
    return first.captured != second.captured ? first.captured < second.captured : pathOrder();
  case LargestFirst:
    return first.bytes != second.bytes ? first.bytes > second.bytes : pathOrder();
  case SmallestFirst:
    return first.bytes != second.bytes ? first.bytes < second.bytes : pathOrder();
  case NameAscending:
    if (const int names = QString::compare(first.fileName, second.fileName, Qt::CaseInsensitive);
        names != 0) {
      return names < 0;
    }
    return pathOrder();
  case NewestFirst:
  default:
    return first.captured != second.captured ? first.captured > second.captured : pathOrder();
  }
}

void CaptureFilterModel::rebuildDuplicateOrdinals() {
  m_duplicateOrdinals.clear();
  if (!sourceModel() || m_duplicateGroups.isEmpty()) {
    return;
  }

  // Each set is represented by the first member under the selected sort. The
  // sets therefore stay together without making Newest, Oldest or Name lie
  // about their order.
  QHash<QString, int> representatives;
  for (int row = 0; row < sourceModel()->rowCount(); ++row) {
    const CaptureRecord& record = sourceRecord(row);
    const QString group = m_duplicateGroups.value(record.path);
    if (group.isEmpty()) {
      continue;
    }
    const auto current = representatives.constFind(group);
    if (current == representatives.cend() ||
        recordLessThan(record, sourceRecord(current.value()))) {
      representatives.insert(group, row);
    }
  }

  QStringList groups = representatives.keys();
  std::sort(groups.begin(), groups.end(), [this, &representatives](const QString& first,
                                                                  const QString& second) {
    const CaptureRecord& a = sourceRecord(representatives.value(first));
    const CaptureRecord& b = sourceRecord(representatives.value(second));
    if (recordLessThan(a, b)) {
      return true;
    }
    if (recordLessThan(b, a)) {
      return false;
    }
    return first < second;
  });
  for (int index = 0; index < groups.size(); ++index) {
    m_duplicateOrdinals.insert(groups.at(index), index + 1);
  }
}

bool CaptureFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
  if (sourceParent.isValid()) {
    return false;
  }
  const CaptureRecord& record = sourceRecord(sourceRow);

  if (!m_showHidden && record.hidden) {
    return false;
  }
  if (m_duplicatesOnly && !m_duplicateGroups.contains(record.path)) {
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
  if (!m_searchTerms.isEmpty()) {
    const QString name = record.fileName.toCaseFolded();
    const QString text = m_ocrFolded.value(record.path);
    for (const QString& term : m_searchTerms) {
      if (!name.contains(term) && !text.contains(term)) {
        return false;
      }
    }
  }
  return true;
}

bool CaptureFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  const CaptureRecord& a = sourceRecord(left.row());
  const CaptureRecord& b = sourceRecord(right.row());
  if (m_duplicatesOnly) {
    const QString firstGroup = m_duplicateGroups.value(a.path);
    const QString secondGroup = m_duplicateGroups.value(b.path);
    if (firstGroup != secondGroup) {
      return m_duplicateOrdinals.value(firstGroup) < m_duplicateOrdinals.value(secondGroup);
    }
  }
  return recordLessThan(a, b);
}

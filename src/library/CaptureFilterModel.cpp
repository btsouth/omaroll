#include "library/CaptureFilterModel.h"

#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QVariantMap>

#include <algorithm>
#include <utility>

CaptureFilterModel::CaptureFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
  // Qt's C-locale collator ignores numeric mode. Keep natural ordering when
  // launched from a minimal environment as well as a localized desktop.
  if (m_nameCollator.locale().language() == QLocale::C) {
    m_nameCollator.setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
  }
  m_nameCollator.setCaseSensitivity(Qt::CaseInsensitive);
  m_nameCollator.setNumericMode(true);
  setDynamicSortFilter(true);
  // lessThan() reads the roles it needs directly, so no single sort role fits;
  // sorting column 0 just gives the proxy something to order.
  sort(0);

  connect(this, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::modelReset, this, &CaptureFilterModel::countChanged);

  m_folderIndexTimer.setSingleShot(true);
  m_folderIndexTimer.setInterval(0);
  connect(&m_folderIndexTimer, &QTimer::timeout, this, [this] {
    const bool folderIndexChanged = rebuildFolderIndex();
    const bool datesChanged = rebuildDateIndex();
    const bool camerasChanged = rebuildCameraIndex();
    if (folderIndexChanged) {
      emit foldersChanged();
    }
    if (datesChanged) {
      emit dateBucketsChanged();
    }
    if (camerasChanged) {
      emit this->camerasChanged();
    }
  });
  m_duplicateOrderTimer.setSingleShot(true);
  m_duplicateOrderTimer.setInterval(0);
  connect(&m_duplicateOrderTimer, &QTimer::timeout, this, [this] {
    if (!m_duplicateGroups.isEmpty() || !m_similarGroups.isEmpty()) {
      rebuildDuplicateOrdinals();
      invalidate();
    }
  });

  m_ocrFilterTimer.setSingleShot(true);
  m_ocrFilterTimer.setInterval(60);
  connect(&m_ocrFilterTimer, &QTimer::timeout, this, [this] {
    beginFilterUpdate();
    endFilterUpdate();
    if (rowCount() > 0) {
      emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {CaptureRoles::OcrSnippetRole});
    }
    emit countChanged();
  });
}

void CaptureFilterModel::setSourceModel(QAbstractItemModel* model) {
  m_folderIndexTimer.stop();
  m_duplicateOrderTimer.stop();
  for (const QMetaObject::Connection& connection : std::as_const(m_sourceConnections)) {
    disconnect(connection);
  }
  m_sourceConnections.clear();

  QSortFilterProxyModel::setSourceModel(model);
  if (!model) {
    m_folders.clear();
    m_folderItemCounts.clear();
    emit foldersChanged();
    m_dateBuckets.clear();
    m_dateDays.clear();
    emit dateBucketsChanged();
    m_cameras.clear();
    m_lenses.clear();
    emit camerasChanged();
    return;
  }

  // A source insertion can be completely filtered out, in which case the
  // proxy emits no row signal even though sourceCount changed.
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged));
  // A scan diff can emit thousands of row signals. Folder choices and their
  // recursive counts need one rebuild after that batch, not one full source
  // walk per inserted, removed, or changed run.
  const auto rebuildFolders = [this] { m_folderIndexTimer.start(); };
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, rebuildFolders));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, rebuildFolders));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::modelReset, this, rebuildFolders));
  const auto rebuildDuplicateOrder = [this] {
    if (!m_duplicateGroups.isEmpty() || !m_similarGroups.isEmpty()) {
      m_duplicateOrderTimer.start();
    }
  };
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, rebuildDuplicateOrder));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, rebuildDuplicateOrder));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::modelReset, this, rebuildDuplicateOrder));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::dataChanged, this,
              [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                if (roles.isEmpty() || roles.contains(CaptureRoles::PathRole)) {
                  m_folderIndexTimer.start();
                }
                if (roles.isEmpty() || roles.contains(CaptureRoles::CapturedRole) ||
                    roles.contains(CaptureRoles::StampRole) ||
                    roles.contains(CaptureRoles::CameraRole) ||
                    roles.contains(CaptureRoles::LensRole)) {
                  m_folderIndexTimer.start();
                }
                if ((!m_duplicateGroups.isEmpty() || !m_similarGroups.isEmpty()) &&
                    (roles.isEmpty() || roles.contains(CaptureRoles::PathRole) ||
                     roles.contains(CaptureRoles::FileNameRole) ||
                     roles.contains(CaptureRoles::CapturedRole) ||
                     roles.contains(CaptureRoles::BytesRole))) {
                  m_duplicateOrderTimer.start();
                }
              }));
  (void)rebuildFolderIndex();
  (void)rebuildDateIndex();
  (void)rebuildCameraIndex();
  emit foldersChanged();
  emit dateBucketsChanged();
  emit camerasChanged();
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
  clearSmartCollection();
  emit kindFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setSortMode(int mode) {
  if (m_sortMode == mode) {
    return;
  }
  m_sortMode = mode;
  m_duplicateOrderTimer.stop();
  rebuildDuplicateOrdinals();
  invalidate();
  clearSmartCollection();
  emit sortModeChanged();
}

void CaptureFilterModel::setSearchText(const QString& text) {
  if (m_searchText == text) {
    return;
  }
  beginFilterUpdate();
  m_searchText = text;
  m_searchTerms =
      text.toCaseFolded().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  endFilterUpdate();
  clearSmartCollection();
  if (rowCount() > 0) {
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {CaptureRoles::OcrSnippetRole});
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
  clearSmartCollection();
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
  clearSmartCollection();
  emit folderFilterChanged();
  emit countChanged();
}

QStringList CaptureFilterModel::folders() const { return m_folders; }

int CaptureFilterModel::folderItemCount(const QString& folder) const {
  return m_folderItemCounts.value(QDir::cleanPath(folder));
}

bool CaptureFilterModel::rebuildFolderIndex() {
  QSet<QString> unique;
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const QString path =
          sourceModel()->data(sourceModel()->index(row, 0), CaptureRoles::PathRole).toString();
      if (!path.isEmpty()) {
        unique.insert(QFileInfo(path).absolutePath());
      }
    }
  }
  QStringList folders(unique.begin(), unique.end());
  std::sort(folders.begin(), folders.end(), [](const QString& left, const QString& right) {
    const int folded = QString::compare(left, right, Qt::CaseInsensitive);
    return folded != 0 ? folded < 0 : left < right;
  });

  QHash<QString, int> counts;
  counts.reserve(unique.size());
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const QString path =
          sourceModel()->data(sourceModel()->index(row, 0), CaptureRoles::PathRole).toString();
      QString directory = QFileInfo(path).absolutePath();
      while (!directory.isEmpty()) {
        // Source roots can contain media only in descendants and therefore
        // are not necessarily in the visible folder list. Keep ancestor
        // counts too so source chips and empty-state diagnostics stay honest.
        ++counts[directory];
        const QString parent = QFileInfo(directory).path();
        if (parent == directory || parent == QStringLiteral(".")) {
          break;
        }
        directory = parent;
      }
    }
  }

  if (m_folders == folders && m_folderItemCounts == counts) {
    return false;
  }
  m_folders = std::move(folders);
  m_folderItemCounts = std::move(counts);
  return true;
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
  clearSmartCollection();
  emit albumFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setTagFilter(const QString& name, const QStringList& paths) {
  const QSet<QString> next(paths.begin(), paths.end());
  if (m_tagFilter == name && m_tagPaths == next) {
    return;
  }
  const bool changedName = m_tagFilter != name;
  beginFilterUpdate();
  m_tagFilter = name;
  m_tagPaths = next;
  endFilterUpdate();
  if (changedName) {
    clearSmartCollection();
  }
  emit tagFilterChanged();
  emit countChanged();
}

namespace {
QDate isoDate(const QString& value) {
  return value.isEmpty() ? QDate() : QDate::fromString(value, Qt::ISODate);
}
} // namespace

void CaptureFilterModel::setDateFrom(const QString& value) {
  const QDate parsed = isoDate(value);
  if (parsed == m_dateFrom && m_modifiedAfter.isEmpty()) {
    return;
  }
  beginFilterUpdate();
  m_dateFrom = parsed;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::setDateTo(const QString& value) {
  const QDate parsed = isoDate(value);
  if (parsed == m_dateTo && m_modifiedAfter.isEmpty()) {
    return;
  }
  beginFilterUpdate();
  m_dateTo = parsed;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::setDateField(int value) {
  const int bounded = qBound(0, value, 1);
  if (bounded == m_dateField && m_modifiedAfter.isEmpty()) {
    return;
  }
  beginFilterUpdate();
  m_dateField = bounded;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::setDateRange(const QString& from, const QString& to, int field) {
  const QDate nextFrom = isoDate(from);
  const QDate nextTo = isoDate(to);
  const int nextField = qBound(0, field, 1);
  if (nextFrom == m_dateFrom && nextTo == m_dateTo && nextField == m_dateField &&
      m_modifiedAfter.isEmpty()) {
    return;
  }
  beginFilterUpdate();
  m_dateFrom = nextFrom;
  m_dateTo = nextTo;
  m_dateField = nextField;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::setModifiedAfter(const QString& value) {
  const QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
  const QString normalized = parsed.isValid() ? parsed.toString(Qt::ISODate) : QString();
  if (normalized == m_modifiedAfter && !m_dateFrom.isValid() && !m_dateTo.isValid()) {
    return;
  }
  beginFilterUpdate();
  m_dateFrom = {};
  m_dateTo = {};
  m_dateField = parsed.isValid() ? 1 : 0;
  m_modifiedAfter = normalized;
  m_modifiedAfterMs = parsed.isValid() ? parsed.toMSecsSinceEpoch() : 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::clearDateRange() {
  if (!m_modifiedAfter.isEmpty()) {
    setModifiedAfter({});
  } else {
    setDateRange({}, {}, 0);
  }
}

bool CaptureFilterModel::rebuildDateIndex() {
  QMap<QDate, int> dayCounts;
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const QDate date = sourceRecord(row).captured.date();
      if (date.isValid()) {
        ++dayCounts[date];
      }
    }
  }

  QMap<QString, int> monthCounts;
  QHash<QString, QVariantList> daysByMonth;
  QList<QDate> dates = dayCounts.keys();
  std::reverse(dates.begin(), dates.end());
  for (const QDate& date : std::as_const(dates)) {
    const QString monthKey = date.toString(QStringLiteral("yyyy-MM"));
    monthCounts[monthKey] += dayCounts.value(date);
    QVariantMap row;
    row.insert(QStringLiteral("date"), date.toString(Qt::ISODate));
    row.insert(QStringLiteral("label"),
               QLocale::system().toString(date, QStringLiteral("dddd, d MMMM")));
    row.insert(QStringLiteral("count"), dayCounts.value(date));
    daysByMonth[monthKey].append(row);
  }

  QVariantList buckets;
  QStringList monthKeys = monthCounts.keys();
  std::reverse(monthKeys.begin(), monthKeys.end());
  for (const QString& key : std::as_const(monthKeys)) {
    const QDate start = QDate::fromString(key + QStringLiteral("-01"), Qt::ISODate);
    QVariantMap row;
    row.insert(QStringLiteral("key"), key);
    row.insert(QStringLiteral("label"),
               QLocale::system().toString(start, QStringLiteral("MMMM yyyy")));
    row.insert(QStringLiteral("count"), monthCounts.value(key));
    row.insert(QStringLiteral("from"), start.toString(Qt::ISODate));
    row.insert(QStringLiteral("to"), start.addMonths(1).addDays(-1).toString(Qt::ISODate));
    buckets.append(row);
  }
  if (buckets == m_dateBuckets && daysByMonth == m_dateDays) {
    return false;
  }
  m_dateBuckets = std::move(buckets);
  m_dateDays = std::move(daysByMonth);
  return true;
}

QVariantList CaptureFilterModel::dateDays(const QString& monthKey) const {
  return m_dateDays.value(monthKey);
}

namespace {

QVariantList countedChoices(const QHash<QString, int>& counts) {
  QStringList names = counts.keys();
  std::sort(names.begin(), names.end(), [&counts](const QString& a, const QString& b) {
    if (counts.value(a) != counts.value(b)) {
      return counts.value(a) > counts.value(b);
    }
    return a.compare(b, Qt::CaseInsensitive) < 0;
  });
  QVariantList rows;
  rows.reserve(names.size());
  for (const QString& name : std::as_const(names)) {
    QVariantMap row;
    row.insert(QStringLiteral("name"), name);
    row.insert(QStringLiteral("count"), counts.value(name));
    rows.append(row);
  }
  return rows;
}

} // namespace

bool CaptureFilterModel::rebuildCameraIndex() {
  QHash<QString, int> cameraCounts;
  QHash<QString, int> lensCounts;
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const CaptureRecord& record = sourceRecord(row);
      if (!record.camera.isEmpty()) {
        ++cameraCounts[record.camera];
      }
      if (!record.lens.isEmpty()) {
        ++lensCounts[record.lens];
      }
    }
  }
  QVariantList cameras = countedChoices(cameraCounts);
  QVariantList lenses = countedChoices(lensCounts);
  if (cameras == m_cameras && lenses == m_lenses) {
    return false;
  }
  m_cameras = std::move(cameras);
  m_lenses = std::move(lenses);
  return true;
}

void CaptureFilterModel::setCameraFilter(const QString& camera) {
  if (m_cameraFilter == camera) {
    return;
  }
  beginFilterUpdate();
  m_cameraFilter = camera;
  endFilterUpdate();
  clearSmartCollection();
  emit cameraFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setLensFilter(const QString& lens) {
  if (m_lensFilter == lens) {
    return;
  }
  beginFilterUpdate();
  m_lensFilter = lens;
  endFilterUpdate();
  clearSmartCollection();
  emit lensFilterChanged();
  emit countChanged();
}

QVariantMap CaptureFilterModel::currentView() const {
  QVariantMap view;
  view.insert(QStringLiteral("search"), m_searchText);
  view.insert(QStringLiteral("kind"), m_kindFilter);
  view.insert(QStringLiteral("folder"), m_folderFilter);
  view.insert(QStringLiteral("favorites"), m_favoritesOnly);
  view.insert(QStringLiteral("showHidden"), m_showHidden);
  view.insert(QStringLiteral("dateFrom"), dateFrom());
  view.insert(QStringLiteral("dateTo"), dateTo());
  view.insert(QStringLiteral("dateField"), m_dateField);
  view.insert(QStringLiteral("modifiedAfter"), m_modifiedAfter);
  view.insert(QStringLiteral("tag"), m_tagFilter);
  view.insert(QStringLiteral("camera"), m_cameraFilter);
  view.insert(QStringLiteral("lens"), m_lensFilter);
  view.insert(QStringLiteral("sort"), m_sortMode);
  return view;
}

void CaptureFilterModel::applyView(const QString& name, const QVariantMap& view,
                                   const QStringList& tagPaths) {
  beginFilterUpdate();
  m_searchText = view.value(QStringLiteral("search")).toString();
  m_searchTerms = m_searchText.toCaseFolded().split(QRegularExpression(QStringLiteral("\\s+")),
                                                    Qt::SkipEmptyParts);
  m_kindFilter = qBound(kAllKinds, view.value(QStringLiteral("kind"), kAllKinds).toInt(),
                        static_cast<int>(CaptureRecord::Document));
  m_folderFilter = view.value(QStringLiteral("folder")).toString();
  m_favoritesOnly = view.value(QStringLiteral("favorites")).toBool();
  m_showHidden = view.value(QStringLiteral("showHidden")).toBool();
  m_dateFrom = isoDate(view.value(QStringLiteral("dateFrom")).toString());
  m_dateTo = isoDate(view.value(QStringLiteral("dateTo")).toString());
  m_dateField = qBound(0, view.value(QStringLiteral("dateField")).toInt(), 1);
  m_modifiedAfter = view.value(QStringLiteral("modifiedAfter")).toString();
  m_modifiedAfterMs = QDateTime::fromString(m_modifiedAfter, Qt::ISODate).toMSecsSinceEpoch();
  m_tagFilter = view.value(QStringLiteral("tag")).toString();
  m_tagPaths = QSet<QString>(tagPaths.begin(), tagPaths.end());
  m_cameraFilter = view.value(QStringLiteral("camera")).toString();
  m_lensFilter = view.value(QStringLiteral("lens")).toString();
  m_sortMode = qBound(0, view.value(QStringLiteral("sort"), NewestFirst).toInt(),
                      static_cast<int>(NameAscending));
  m_albumFilter.clear();
  m_albumPaths.clear();
  m_duplicatesOnly = false;
  m_similarOnly = false;
  m_smartCollectionFilter = name;
  endFilterUpdate();
  invalidate();
  emit searchTextChanged();
  emit kindFilterChanged();
  emit folderFilterChanged();
  emit favoritesOnlyChanged();
  emit showHiddenChanged();
  emit dateRangeChanged();
  emit tagFilterChanged();
  emit cameraFilterChanged();
  emit lensFilterChanged();
  emit sortModeChanged();
  emit albumFilterChanged();
  emit duplicatesOnlyChanged();
  emit similarOnlyChanged();
  emit smartCollectionFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::clearSmartCollection() {
  if (m_smartCollectionFilter.isEmpty()) {
    return;
  }
  m_smartCollectionFilter.clear();
  emit smartCollectionFilterChanged();
}

void CaptureFilterModel::setShowHidden(bool value) {
  if (m_showHidden == value) {
    return;
  }
  beginFilterUpdate();
  m_showHidden = value;
  endFilterUpdate();
  clearSmartCollection();
  emit showHiddenChanged();
  emit countChanged();
}

void CaptureFilterModel::setDuplicatesOnly(bool value) {
  if (m_duplicatesOnly == value) {
    return;
  }
  beginFilterUpdate();
  m_duplicatesOnly = value;
  if (value) {
    m_similarOnly = false;
  }
  endFilterUpdate();
  rebuildDuplicateOrdinals();
  invalidate();
  clearSmartCollection();
  emit duplicatesOnlyChanged();
  if (value) {
    emit similarOnlyChanged();
  }
  emit countChanged();
}

void CaptureFilterModel::setSimilarOnly(bool value) {
  if (m_similarOnly == value) {
    return;
  }
  beginFilterUpdate();
  m_similarOnly = value;
  if (value) {
    m_duplicatesOnly = false;
  }
  endFilterUpdate();
  rebuildDuplicateOrdinals();
  invalidate();
  clearSmartCollection();
  emit similarOnlyChanged();
  if (value) {
    emit duplicatesOnlyChanged();
  }
  emit countChanged();
}

void CaptureFilterModel::setDuplicateGroups(const QHash<QString, QString>& groups) {
  if (m_duplicateGroups == groups) {
    return;
  }

  beginFilterUpdate();
  m_duplicateGroups = groups;
  m_duplicateOrderTimer.stop();
  rebuildDuplicateOrdinals();
  endFilterUpdate();
  invalidate();
  emit countChanged();
}

void CaptureFilterModel::setSimilarGroups(const QHash<QString, QString>& groups) {
  if (m_similarGroups == groups) {
    return;
  }
  beginFilterUpdate();
  m_similarGroups = groups;
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
  if (!m_duplicatesOnly && !m_similarOnly) {
    return dayLabelAt(row);
  }
  const QHash<QString, QString>& groups = m_similarOnly ? m_similarGroups : m_duplicateGroups;
  const QString group = groups.value(pathAt(row));
  const int ordinal = m_duplicateOrdinals.value(group);
  if (ordinal <= 0) {
    return {};
  }
  return QStringLiteral("%1 %2 of %3")
      .arg(m_similarOnly ? QStringLiteral("Similar set") : QStringLiteral("Exact match"))
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

bool CaptureFilterModel::isDocumentAt(int row) const {
  return data(index(row, 0), CaptureRoles::IsDocumentRole).toBool();
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
    if (const int names = m_nameCollator.compare(first.fileName, second.fileName);
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
  const QHash<QString, QString>& reviewGroups = m_similarOnly ? m_similarGroups : m_duplicateGroups;
  if (!sourceModel() || reviewGroups.isEmpty()) {
    return;
  }

  // Each set is represented by the first member under the selected sort. The
  // sets therefore stay together without making Newest, Oldest or Name lie
  // about their order.
  QHash<QString, int> representatives;
  for (int row = 0; row < sourceModel()->rowCount(); ++row) {
    const CaptureRecord& record = sourceRecord(row);
    const QString group = reviewGroups.value(record.path);
    if (group.isEmpty()) {
      continue;
    }
    const auto current = representatives.constFind(group);
    if (current == representatives.cend() ||
        recordLessThan(record, sourceRecord(current.value()))) {
      representatives.insert(group, row);
    }
  }

  QStringList groupOrder = representatives.keys();
  std::sort(groupOrder.begin(), groupOrder.end(),
            [this, &representatives](const QString& first, const QString& second) {
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
  for (int index = 0; index < groupOrder.size(); ++index) {
    m_duplicateOrdinals.insert(groupOrder.at(index), index + 1);
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
  if (m_similarOnly && !m_similarGroups.contains(record.path)) {
    return false;
  }
  if (m_favoritesOnly && !record.favorite) {
    return false;
  }
  if (!m_albumFilter.isEmpty() && !m_albumPaths.contains(record.path)) {
    return false;
  }
  if (!m_tagFilter.isEmpty() && !m_tagPaths.contains(record.path)) {
    return false;
  }
  if (!m_cameraFilter.isEmpty() && record.camera != m_cameraFilter) {
    return false;
  }
  if (!m_lensFilter.isEmpty() && record.lens != m_lensFilter) {
    return false;
  }
  if (m_modifiedAfterMs > 0 && record.modified <= m_modifiedAfterMs) {
    return false;
  }
  const QDate date = m_dateField == 1 ? QDateTime::fromMSecsSinceEpoch(record.modified).date()
                                      : record.captured.date();
  if (m_dateFrom.isValid() && date < m_dateFrom) {
    return false;
  }
  if (m_dateTo.isValid() && date > m_dateTo) {
    return false;
  }
  if (!m_folderFilter.isEmpty()) {
    const QString directory = QFileInfo(record.path).absolutePath();
    const QString prefix = m_folderFilter.endsWith(QLatin1Char('/'))
                               ? m_folderFilter
                               : m_folderFilter + QLatin1Char('/');
    if (directory != m_folderFilter && !directory.startsWith(prefix)) {
      return false;
    }
  }
  if (m_kindFilter != kAllKinds) {
    if (m_kindFilter == CaptureRecord::Document) {
      if (!record.isDocument()) {
        return false;
      }
    } else if (static_cast<int>(record.kind) != m_kindFilter) {
      return false;
    }
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
  if (m_duplicatesOnly || m_similarOnly) {
    const QHash<QString, QString>& groups = m_similarOnly ? m_similarGroups : m_duplicateGroups;
    const QString firstGroup = groups.value(a.path);
    const QString secondGroup = groups.value(b.path);
    if (firstGroup != secondGroup) {
      return m_duplicateOrdinals.value(firstGroup) < m_duplicateOrdinals.value(secondGroup);
    }
  }
  return recordLessThan(a, b);
}

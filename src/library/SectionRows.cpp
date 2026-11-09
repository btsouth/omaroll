#include "library/SectionRows.h"

#include "library/CaptureFilterModel.h"
#include "library/CaptureRoles.h"

#include <QDateTime>
#include <QLocale>

#include <algorithm>

namespace {

QString itemCount(int count) {
  return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? QStringLiteral("item")
                                                           : QStringLiteral("items"));
}

bool sameMonth(const QDate& first, const QDate& second) {
  if (!first.isValid() || !second.isValid()) {
    return first.isValid() == second.isValid();
  }
  return first.year() == second.year() && first.month() == second.month();
}

} // namespace

SectionRows::SectionRows(CaptureFilterModel* source) : QAbstractListModel(source), m_source(source) {
  m_rebuild.setSingleShot(true);
  m_rebuild.setInterval(0);
  connect(&m_rebuild, &QTimer::timeout, this, &SectionRows::rebuildNow);

  const auto changed = [this] { scheduleRebuild(); };
  connect(source, &QAbstractItemModel::rowsInserted, this, changed);
  connect(source, &QAbstractItemModel::rowsRemoved, this, changed);
  connect(source, &QAbstractItemModel::rowsMoved, this, changed);
  connect(source, &QAbstractItemModel::layoutChanged, this, changed);
  connect(source, &QAbstractItemModel::modelReset, this, changed);
  connect(source, &QAbstractItemModel::dataChanged, this,
          [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            // Marks, ratings and thumbnails do not move a section boundary.
            if (roles.isEmpty() || roles.contains(CaptureRoles::CapturedRole) ||
                roles.contains(CaptureRoles::DayLabelRole)) {
              scheduleRebuild();
            }
          });
  const auto modeMayChange = [this] {
    const int now = mode();
    if (now != m_lastMode) {
      m_lastMode = now;
      emit modeChanged();
    }
    scheduleRebuild();
  };
  connect(source, &CaptureFilterModel::sortModeChanged, this, modeMayChange);
  connect(source, &CaptureFilterModel::similarOnlyChanged, this, modeMayChange);
  connect(source, &CaptureFilterModel::duplicatesOnlyChanged, this, modeMayChange);
  m_lastMode = mode();
}

int SectionRows::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant SectionRows::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
    return {};
  }
  const Row& row = m_rows.at(index.row());
  const Section& section = row.section;
  switch (role) {
  case HeaderRole:
    return row.header;
  case FirstRole:
    return row.first;
  case CountRole:
    return row.count;
  case LabelRole:
  case Qt::DisplayRole:
    return section.label;
  case DetailRole:
    return sectionDetail(section);
  case SectionFirstRole:
    return section.first;
  case SectionCountRole:
    return section.count;
  case MonthRole:
    return section.month;
  default:
    return {};
  }
}

QHash<int, QByteArray> SectionRows::roleNames() const {
  return {
      {HeaderRole, "header"},
      {FirstRole, "first"},
      {CountRole, "count"},
      {LabelRole, "label"},
      {DetailRole, "detail"},
      {SectionFirstRole, "sectionFirst"},
      {SectionCountRole, "sectionCount"},
      {MonthRole, "month"},
  };
}

void SectionRows::setActive(bool active) {
  if (m_active == active) return;
  m_active = active;
  emit activeChanged();
  scheduleRebuild();
}

void SectionRows::setColumns(int columns) {
  columns = std::max(1, columns);
  if (m_columns == columns) return;
  m_columns = columns;
  emit columnsChanged();
  scheduleRebuild();
}

void SectionRows::setCompact(bool compact) {
  if (m_compact == compact) return;
  m_compact = compact;
  emit compactChanged();
  scheduleRebuild();
}

int SectionRows::mode() const {
  if (m_source->similarOnly()) return ReviewSections;
  if (m_source->duplicatesOnly()) return NoSections;
  const int sort = m_source->sortMode();
  return sort == CaptureFilterModel::NewestFirst || sort == CaptureFilterModel::OldestFirst
             ? DateSections
             : NoSections;
}

void SectionRows::scheduleRebuild() {
  if (m_active || !m_rows.isEmpty()) m_rebuild.start();
}

QList<SectionRows::Section> SectionRows::computeSections() const {
  QList<Section> sections;
  const int currentMode = mode();
  if (currentMode == ReviewSections) {
    for (const QVariant& value : m_source->reviewSections()) {
      const QVariantMap map = value.toMap();
      sections.append({map.value(QStringLiteral("first")).toInt(),
                       map.value(QStringLiteral("count")).toInt(),
                       map.value(QStringLiteral("label")).toString(), false});
    }
    return sections;
  }
  if (currentMode != DateSections) return sections;

  // The captured moment is what the sort uses: EXIF original dates once
  // indexed, the producer's stamp or mtime before. Its calendar day in the
  // local zone is the day the tile and the day label name.
  struct Day {
    int first;
    int count;
    QDate date;
  };
  QList<Day> days;
  const int rows = m_source->rowCount();
  for (int row = 0; row < rows; ++row) {
    const QDate date =
        m_source->data(m_source->index(row, 0), CaptureRoles::CapturedRole).toDateTime().date();
    if (days.isEmpty() || days.last().date != date) {
      days.append({row, 1, date});
    } else {
      ++days.last().count;
    }
  }

  const QLocale locale = QLocale::system();
  for (qsizetype start = 0; start < days.size();) {
    qsizetype end = start;
    int items = 0;
    while (end < days.size() && sameMonth(days.at(end).date, days.at(start).date)) {
      items += days.at(end).count;
      ++end;
    }
    const auto dayCount = static_cast<int>(end - start);
    // A month of thin days reads better as one section than as a header over
    // every row or two: compact tiles, or under half a row per day.
    const bool monthly = m_compact || items * 2 < dayCount * m_columns;
    const QDate date = days.at(start).date;
    if (!date.isValid()) {
      sections.append({days.at(start).first, items, QStringLiteral("Unknown date"), true});
    } else if (monthly) {
      sections.append({days.at(start).first, items,
                       locale.toString(date, QStringLiteral("MMMM yyyy")), true});
    } else {
      for (qsizetype day = start; day < end; ++day) {
        const Day& run = days.at(day);
        sections.append(
            {run.first, run.count,
             m_source->data(m_source->index(run.first, 0), CaptureRoles::DayLabelRole).toString(),
             false});
      }
    }
    start = end;
  }
  return sections;
}

void SectionRows::rebuildNow() {
  m_rebuild.stop();
  apply(m_active ? computeSections() : QList<Section>());
  emit rebuilt();
}

void SectionRows::apply(QList<Section> sections) {
  QList<Row> rows;
  for (qsizetype index = 0; index < sections.size(); ++index) {
    const Section& section = sections.at(index);
    rows.append({true, section.first, section.count, section});
    for (int offset = 0; offset < section.count; offset += m_columns) {
      rows.append({false, section.first + offset, std::min(m_columns, section.count - offset),
                   section});
    }
  }

  // Report only what differs, so delegates on screen keep their place: the
  // common prefix and suffix stay, the middle changes in place, and rows are
  // inserted or removed only at its end.
  const auto same = [&](qsizetype before, qsizetype after) {
    return m_rows.at(before) == rows.at(after);
  };
  const qsizetype oldCount = m_rows.size();
  const qsizetype newCount = rows.size();
  qsizetype prefix = 0;
  while (prefix < oldCount && prefix < newCount && same(prefix, prefix)) ++prefix;
  qsizetype suffix = 0;
  while (suffix < oldCount - prefix && suffix < newCount - prefix &&
         same(oldCount - 1 - suffix, newCount - 1 - suffix)) {
    ++suffix;
  }
  const qsizetype oldMiddle = oldCount - prefix - suffix;
  const qsizetype newMiddle = newCount - prefix - suffix;
  const qsizetype common = std::min(oldMiddle, newMiddle);

  m_sections = std::move(sections);
  const auto changeCommon = [&] {
    if (common > 0) {
      emit dataChanged(index(static_cast<int>(prefix)),
                       index(static_cast<int>(prefix + common - 1)));
    }
  };
  if (newMiddle < oldMiddle) {
    // Drop the surplus first; the rows left keep their old reading until
    // they are changed in place.
    beginRemoveRows({}, static_cast<int>(prefix + common),
                    static_cast<int>(prefix + oldMiddle - 1));
    m_rows.remove(prefix + common, oldMiddle - common);
    endRemoveRows();
    m_rows = std::move(rows);
    changeCommon();
  } else if (newMiddle > oldMiddle) {
    // Change the rows both lists have, then add the rest after them.
    for (qsizetype row = prefix; row < prefix + common; ++row) m_rows[row] = rows.at(row);
    changeCommon();
    beginInsertRows({}, static_cast<int>(prefix + oldMiddle),
                    static_cast<int>(prefix + newMiddle - 1));
    m_rows = std::move(rows);
    endInsertRows();
  } else {
    m_rows = std::move(rows);
    changeCommon();
  }
}

QString SectionRows::sectionDetail(const Section& section) const {
  return itemCount(section.count);
}

int SectionRows::rowOfItem(int item) const {
  if (item < 0 || m_rows.isEmpty()) return -1;
  // Rows are in library order; find the last row starting at or before item.
  const auto after = std::upper_bound(m_rows.cbegin(), m_rows.cend(), item,
                                      [](int value, const Row& row) { return value < row.first; });
  for (auto it = after; it != m_rows.cbegin();) {
    --it;
    if (it->header) continue;
    return item < it->first + it->count ? static_cast<int>(it - m_rows.cbegin()) : -1;
  }
  return -1;
}

QVariantMap SectionRows::sectionOfItem(int item) const {
  const int row = rowOfItem(item);
  if (row < 0) return {};
  const Section& section = m_rows.at(row).section;
  int header = row;
  while (header > 0 && !m_rows.at(header).header) --header;
  return {{QStringLiteral("first"), section.first},
          {QStringLiteral("count"), section.count},
          {QStringLiteral("label"), section.label},
          {QStringLiteral("month"), section.month},
          {QStringLiteral("headerRow"), header}};
}

QVariantMap SectionRows::rowAt(int row) const {
  if (row < 0 || row >= m_rows.size()) return {};
  const Row& at = m_rows.at(row);
  return {{QStringLiteral("header"), at.header},
          {QStringLiteral("first"), at.first},
          {QStringLiteral("count"), at.count}};
}

QVariantList SectionRows::sections() const {
  QVariantList list;
  for (const Section& section : m_sections) {
    list.append(QVariantMap{{QStringLiteral("first"), section.first},
                            {QStringLiteral("count"), section.count},
                            {QStringLiteral("label"), section.label},
                            {QStringLiteral("month"), section.month}});
  }
  return list;
}

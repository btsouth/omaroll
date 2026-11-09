#include "library/SectionRows.h"

#include "library/CaptureFilterModel.h"
#include "library/CaptureRoles.h"

#include <QDateTime>
#include <QLocale>
#include <QSet>

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

SectionRows::RowKey SectionRows::keyOf(const Row& row) {
  return {row.section.label, row.header ? -1 : row.first - row.section.first};
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
  m_sections = std::move(sections);

  // Walk both lists matching rows by key. A row only the old list has is
  // removed where it sits, a row only the new list has is inserted there,
  // and a matched row whose contents moved is changed in place. Keys found
  // in both lists but in another order are removed and inserted again.
  const QList<Row> old = m_rows;
  const qsizetype oldCount = old.size();
  const qsizetype newCount = rows.size();
  QSet<RowKey> oldLeft;
  QSet<RowKey> newLeft;
  oldLeft.reserve(oldCount);
  newLeft.reserve(newCount);
  for (const Row& row : old) oldLeft.insert(keyOf(row));
  for (const Row& row : rows) newLeft.insert(keyOf(row));

  enum Step { Match, Remove, Insert };
  qsizetype from = 0;
  qsizetype to = 0;
  const auto step = [&]() {
    if (from >= oldCount) return Insert;
    if (to >= newCount) return Remove;
    const RowKey before = keyOf(old.at(from));
    const RowKey after = keyOf(rows.at(to));
    if (before == after) return Match;
    if (!newLeft.contains(before) || oldLeft.contains(after)) return Remove;
    return Insert;
  };
  QList<qsizetype> changed;
  while (from < oldCount || to < newCount) {
    switch (step()) {
    case Match: {
      if (!(old.at(from) == rows.at(to))) changed.append(to);
      const RowKey key = keyOf(old.at(from));
      oldLeft.remove(key);
      newLeft.remove(key);
      ++from;
      ++to;
      break;
    }
    case Remove: {
      // The rows before `to` already match the new list, so the old row
      // sits at `to` in m_rows.
      const qsizetype start = from;
      while (from < oldCount && step() == Remove) oldLeft.remove(keyOf(old.at(from++)));
      const auto first = static_cast<int>(to);
      beginRemoveRows({}, first, first + static_cast<int>(from - start) - 1);
      m_rows.remove(to, from - start);
      endRemoveRows();
      break;
    }
    case Insert: {
      const qsizetype start = to;
      while (to < newCount && step() == Insert) newLeft.remove(keyOf(rows.at(to++)));
      beginInsertRows({}, static_cast<int>(start), static_cast<int>(to) - 1);
      m_rows.insert(start, to - start, Row{});
      for (qsizetype row = start; row < to; ++row) m_rows[row] = rows.at(row);
      endInsertRows();
      break;
    }
    }
  }

  m_rows = std::move(rows);
  m_headersBefore.resize(m_rows.size() + 1);
  m_headersBefore[0] = 0;
  for (qsizetype row = 0; row < m_rows.size(); ++row) {
    m_headersBefore[row + 1] = m_headersBefore.at(row) + (m_rows.at(row).header ? 1 : 0);
  }
  for (qsizetype at = 0; at < changed.size();) {
    qsizetype end = at;
    while (end + 1 < changed.size() && changed.at(end + 1) == changed.at(end) + 1) ++end;
    emit dataChanged(index(static_cast<int>(changed.at(at))),
                     index(static_cast<int>(changed.at(end))));
    at = end + 1;
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

int SectionRows::sectionFirstOf(int item) const {
  if (item < 0 || m_sections.isEmpty()) return -1;
  const auto after = std::upper_bound(
      m_sections.cbegin(), m_sections.cend(), item,
      [](int value, const Section& section) { return value < section.first; });
  if (after == m_sections.cbegin()) return -1;
  const Section& section = *(after - 1);
  return item < section.first + section.count ? section.first : -1;
}

double SectionRows::rowY(int row, double headerHeight, double rowHeight) const {
  // m_headersBefore follows m_rows once a rebuild has finished.
  row = std::clamp(row, 0, static_cast<int>(m_headersBefore.size()) - 1);
  const int headers = m_headersBefore.at(row);
  return headers * headerHeight + (row - headers) * rowHeight;
}

int SectionRows::rowAtY(double y, double headerHeight, double rowHeight) const {
  const auto rows = static_cast<int>(m_headersBefore.size()) - 1;
  if (rows <= 0) return -1;
  // The first row whose end lies below y.
  int low = 0;
  int high = rows - 1;
  while (low < high) {
    const int middle = low + (high - low) / 2;
    if (rowY(middle + 1, headerHeight, rowHeight) <= y) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

double SectionRows::totalHeight(double headerHeight, double rowHeight) const {
  return rowY(static_cast<int>(m_headersBefore.size()) - 1, headerHeight, rowHeight);
}

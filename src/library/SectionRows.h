#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <utility>

class CaptureFilterModel;

// The library laid out as rows for a sectioned view: a header row per section,
// then the section's tiles in rows of `columns`. A GridView has no sections,
// so the view lists these rows instead and lays the tiles of each row out
// itself.
//
// Sections follow the view's order: in a date sort, one per day, or one per
// month when tiles are compact or the month's days hold less than half a row
// each on average; while reviewing similar pictures, one per set. Other sorts
// have no sections.
//
// Rows are rebuilt on the next event-loop turn after the library changes.
// Rows are matched by section and place within it, so a capture added to or
// removed from one section is reported as rows inserted or removed there, and
// a delegate on screen keeps its place.
//
// Header and tile rows have different heights; the geometry functions give
// each row's exact offset so the scrollbar and jumps can map a position to the
// same row every time, rather than relying on the list's estimate.
class SectionRows final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
  Q_PROPERTY(int columns READ columns WRITE setColumns NOTIFY columnsChanged)
  Q_PROPERTY(bool compact READ compact WRITE setCompact NOTIFY compactChanged)
  Q_PROPERTY(int mode READ mode NOTIFY modeChanged)
  Q_PROPERTY(int sectionCount READ sectionCount NOTIFY rebuilt)

public:
  enum Mode { NoSections, DateSections, ReviewSections };
  Q_ENUM(Mode)

  enum Role {
    HeaderRole = Qt::UserRole + 1,
    FirstRole,
    CountRole,
    LabelRole,
    DetailRole,
    SectionFirstRole,
    SectionCountRole,
    MonthRole,
  };

  struct Section {
    int first = 0;
    int count = 0;
    QString label;
    bool month = false;
    bool operator==(const Section&) const = default;
  };

  // Each row carries its section, so a row reads the same whichever list
  // it sits in while the rows are being swapped.
  struct Row {
    bool header = false;
    int first = 0;
    int count = 0;
    Section section;
    bool operator==(const Row&) const = default;
  };
  // A row's identity across rebuilds: its section and its tile offset within
  // it, -1 for the header.
  using RowKey = std::pair<QString, int>;
  [[nodiscard]] static RowKey keyOf(const Row& row);

  explicit SectionRows(CaptureFilterModel* source);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  [[nodiscard]] bool active() const { return m_active; }
  void setActive(bool active);
  [[nodiscard]] int columns() const { return m_columns; }
  void setColumns(int columns);
  [[nodiscard]] bool compact() const { return m_compact; }
  void setCompact(bool compact);
  // What the source's current sort and review filter call for.
  [[nodiscard]] int mode() const;
  [[nodiscard]] int sectionCount() const { return static_cast<int>(m_sections.size()); }

  // The list row holding a library row, or -1.
  Q_INVOKABLE int rowOfItem(int item) const;
  // The section holding a library row: first, count, label and its header row.
  Q_INVOKABLE QVariantMap sectionOfItem(int item) const;
  // Header, first and count of one list row, for geometry outside a delegate.
  Q_INVOKABLE QVariantMap rowAt(int row) const;
  Q_INVOKABLE QVariantList sections() const;
  // First library row of the section holding item, or -1.
  Q_INVOKABLE int sectionFirstOf(int item) const;
  // Exact geometry with header rows headerHeight and tile rows rowHeight
  // high: where a row starts, which row covers y, and the total height.
  Q_INVOKABLE double rowY(int row, double headerHeight, double rowHeight) const;
  Q_INVOKABLE int rowAtY(double y, double headerHeight, double rowHeight) const;
  Q_INVOKABLE double totalHeight(double headerHeight, double rowHeight) const;
  // Rebuild now rather than on the next turn.
  Q_INVOKABLE void rebuildNow();

  // Exposed for tests: the sections for the source as it is now.
  [[nodiscard]] QList<Section> computeSections() const;

signals:
  void activeChanged();
  void columnsChanged();
  void compactChanged();
  void modeChanged();
  // After every rebuild, whether or not any row changed.
  void rebuilt();

private:
  void scheduleRebuild();
  void apply(QList<Section> sections);
  [[nodiscard]] QString sectionDetail(const Section& section) const;

  CaptureFilterModel* m_source;
  QTimer m_rebuild;
  QList<Section> m_sections;
  QList<Row> m_rows;
  // Header rows before each row, and one past the end.
  QList<int> m_headersBefore{0};
  bool m_active = false;
  bool m_compact = false;
  int m_columns = 1;
  int m_lastMode = NoSections;
};

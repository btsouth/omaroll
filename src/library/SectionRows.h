#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QTimer>
#include <QVariantMap>

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
// Rows are rebuilt on the next event-loop turn after the library changes, and
// only the rows that differ are reported, so a delegate on screen keeps its
// place and a new capture does not reset the view.
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
  bool m_active = false;
  bool m_compact = false;
  int m_columns = 1;
  int m_lastMode = NoSections;
};

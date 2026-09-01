#pragma once

#include "library/CaptureRecord.h"

#include <QObject>
#include <QStringList>
#include <QVariantList>

class ActionLauncher;

// The delegation matrix, as data.
//
// omaroll performs almost nothing itself. This table says, for each kind of
// capture, which already-installed tool owns the job and how to hand the file
// over. Adding a handler is a row here plus an argv template, not new code.
//
// Nothing is run through a shell. Arguments are passed as a real argv, so a
// filename containing a quote, a space or a semicolon is data rather than
// syntax.
class ActionRegistry final : public QObject {
  Q_OBJECT

public:
  struct Definition {
    QString id;
    QString label;
    // Empty for actions omaroll performs itself; QML handles those and they are
    // always reported as available.
    QString program;
    // "{path}" is substituted with the file. Everything else is literal.
    QStringList arguments;
    QString shortcut;
    // Named in the error when the program is missing, so the message can say
    // what to install rather than just failing.
    QString packageHint;
    QList<CaptureRecord::Kind> kinds;
    // Shown first and bound to Enter for its kind.
    bool primary = false;
  };

  explicit ActionRegistry(ActionLauncher* launcher, QObject* parent = nullptr);

  // Rows for QML: id, label, shortcut, available, hint, primary.
  Q_INVOKABLE QVariantList actionsFor(int kind) const;

  // Runs a delegated action. Returns false for an unknown id, an action omaroll
  // handles itself, or a missing program; the launcher reports why.
  Q_INVOKABLE bool run(const QString& id, const QString& path);

  // True when the action is one QML performs in-app rather than delegating.
  Q_INVOKABLE bool isNative(const QString& id) const;

  Q_INVOKABLE QString primaryActionFor(int kind) const;

private:
  [[nodiscard]] static QString screenshotEditor();
  [[nodiscard]] const Definition* find(const QString& id) const;
  [[nodiscard]] static QList<Definition> buildTable();

  ActionLauncher* m_launcher = nullptr;
  QList<Definition> m_definitions;
};

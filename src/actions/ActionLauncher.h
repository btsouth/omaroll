#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// Hands a capture to whoever already owns the job.
//
// omaroll deliberately performs almost no work itself: this class is the seam
// where the library stops and Omarchy's existing tools take over. Everything
// runs detached with an explicit argv, never through a shell, so a filename
// containing a quote or a semicolon is data rather than syntax.
class ActionLauncher final : public QObject {
  Q_OBJECT

public:
  explicit ActionLauncher(QObject* parent = nullptr);

  // Open in whatever the desktop has registered for the type.
  Q_INVOKABLE bool open(const QString& path);

  // Reveal in the file manager with the file selected.
  Q_INVOKABLE bool showInFiles(const QString& path);

  // True when a program is on PATH. QML uses this to grey out an action rather
  // than failing after the click.
  Q_INVOKABLE bool handlerAvailable(const QString& program) const;

signals:
  // Something the user should be told about, in their words rather than a
  // process exit code.
  void failed(const QString& message);

private:
  bool run(const QString& program, const QStringList& arguments);
};

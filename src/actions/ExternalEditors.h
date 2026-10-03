#pragma once

#include <QList>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantList>

class ActionLauncher;

// RAW-only handoff to a preferred external developer.
//
// The chooser is separate from ActionRegistry because choosing a default is a
// user preference, not another launch row. All process startup still goes
// through ActionLauncher, including its missing-program messages.
class ExternalEditors final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList candidates READ candidates NOTIFY preferencesChanged)
  Q_PROPERTY(
      QString preferredId READ preferredId WRITE setPreferredId NOTIFY preferencesChanged)
  Q_PROPERTY(QString customCommand READ customCommand WRITE setCustomCommand NOTIFY
                 preferencesChanged)
  Q_PROPERTY(bool alwaysChoose READ alwaysChoose WRITE setAlwaysChoose NOTIFY preferencesChanged)
  Q_PROPERTY(QString requestedPath READ requestedPath NOTIFY requestedPathChanged)
  Q_PROPERTY(QString message READ message NOTIFY messageChanged)

public:
  explicit ExternalEditors(ActionLauncher* launcher, QObject* parent = nullptr);

  // id, label, available and preferred for darktable, RawTherapee, ART,
  // digiKam, GIMP and the user's custom command.
  [[nodiscard]] QVariantList candidates() const;

  [[nodiscard]] QString preferredId() const { return m_preferredId; }
  void setPreferredId(const QString& id);
  Q_INVOKABLE bool setPreferred(const QString& id);
  Q_INVOKABLE bool customCommandAvailable(const QString& command) const;

  [[nodiscard]] QString customCommand() const { return m_customCommand; }
  void setCustomCommand(const QString& command);

  [[nodiscard]] bool alwaysChoose() const { return m_alwaysChoose; }
  void setAlwaysChoose(bool value);

  [[nodiscard]] QString requestedPath() const { return m_requestedPath; }
  [[nodiscard]] QString message() const { return m_message; }

  // Opens through the saved preference or the first installed developer. With
  // alwaysChoose, and when no preference is usable, this asks for the dialog.
  Q_INVOKABLE bool requestOpen(const QString& path);

  // Shows the chooser for a path. This is also the signal used by the dialog so
  // direct QML calls and requestOpen share one path.
  Q_INVOKABLE void showChooser(const QString& path);

  // Launches one candidate. Unknown ids and non-RAW files are refused before
  // ActionLauncher sees them.
  Q_INVOKABLE bool launch(const QString& id, const QString& path);

signals:
  void preferencesChanged();
  void requestedPathChanged();
  void messageChanged();
  void chooseRequested();

private:
  struct Editor {
    QString id;
    QString label;
    QString program;
    QString packageHint;
  };

  [[nodiscard]] static QList<Editor> knownEditors();
  [[nodiscard]] static QString executableFor(const QString& program);
  [[nodiscard]] bool editableId(const QString& id) const;
  [[nodiscard]] bool available(const QString& id) const;
  [[nodiscard]] QString firstInstalledId() const;
  [[nodiscard]] bool parseCustomCommand(const QString& command, QString* program,
                                        QStringList* arguments) const;
  void setRequestedPath(const QString& path);
  void setMessage(const QString& message);
  void reject(const QString& message);

  ActionLauncher* m_launcher = nullptr;
  QSettings m_settings;
  QString m_preferredId;
  QString m_customCommand;
  bool m_alwaysChoose = false;
  QString m_requestedPath;
  QString m_message;
};

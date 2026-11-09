#pragma once
#include <QObject>
#include <QStringList>

class MimeDefaults final : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  static QStringList types(const QString &desktopFile, const QString &group);
  // Returns a printable report, including any failures. No UI is required.
  Q_INVOKABLE QString apply(bool unset = false,
                            const QString &group = QStringLiteral("all"));
  bool succeeded() const { return m_succeeded; }

private:
  bool m_succeeded = false;
};

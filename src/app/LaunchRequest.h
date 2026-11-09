#pragma once
#include <QObject>
#include <QHash>
#include <QStringList>
#include <optional>

// A request's explicit position is consumed once, by its first video.
class LaunchRequest final : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  static std::optional<qint64> parseStart(const QString& value);
  void prepare(const QStringList& files, qint64 milliseconds);
  Q_INVOKABLE qint64 pendingStart(const QString& path) const { return m_starts.value(path, -1); }
  Q_INVOKABLE qint64 takeStart(const QString& path) { const auto result = m_starts.value(path, -1); m_starts.remove(path); return result; }
signals:
  void changed();
private:
  QHash<QString, qint64> m_starts;
};

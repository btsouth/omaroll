#pragma once
#include <QString>
#include <QStringList>
#include <optional>

namespace LaunchRequest {
// Milliseconds for seconds, mm:ss or hh:mm:ss, with optional fractions.
std::optional<qint64> parseStart(const QString& value);
// The video --start applies to: the first video of the request.
QString firstVideo(const QStringList& files);
} // namespace LaunchRequest

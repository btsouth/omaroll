#include "app/LaunchRequest.h"
#include "sources/CaptureScanner.h"
#include <QRegularExpression>
#include <cmath>

std::optional<qint64> LaunchRequest::parseStart(const QString &value) {
  static const QRegularExpression pattern(
      QStringLiteral(R"(^\d+(?::\d{1,2}){0,2}(?:\.\d+)?$)"));
  if (!pattern.match(value).hasMatch())
    return {};
  const auto parts = value.split(u':');
  double seconds = 0;
  for (qsizetype i = 0; i < parts.size(); ++i) {
    bool ok;
    const double part = parts[i].toDouble(&ok);
    if (!ok || (i > 0 && part >= 60))
      return {};
    seconds = seconds * 60 + part;
  }
  if (!std::isfinite(seconds) || seconds * 1000 >= 9007199254740991.0)
    return {};
  return qint64(std::round(seconds * 1000));
}
QString LaunchRequest::firstVideo(const QStringList& files) {
  for (const auto& path : files) {
    if (CaptureScanner::isVideo(CaptureScanner::mediaSuffix(path))) {
      return path;
    }
  }
  return {};
}

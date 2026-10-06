#pragma once

#include "sources/FileVersion.h"

#include <QFileInfo>
#include <QCryptographicHash>
#include <QRegularExpression>

namespace ThumbnailSource {

// Transcodes intentionally share the original recording's tile.
inline QString path(const QString& requested) {
  static const QRegularExpression derivedName(
      QStringLiteral(R"(^(.*)-(?:4k|1080p|720p)\.(?:mp4|gif)$)"),
      QRegularExpression::CaseInsensitiveOption);
  const auto derived = derivedName.match(requested);
  if (derived.hasMatch()) {
    for (const QString& extension :
         {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm"),
          QStringLiteral("mov"), QStringLiteral("m4v"), QStringLiteral("avi")}) {
      const QString candidate = derived.captured(1) + QLatin1Char('.') + extension;
      if (QFileInfo(candidate).isFile()) return candidate;
    }
  }
  return requested;
}

inline QString version(const QString& requested, const QString& ownVersion) {
  const QString source = path(requested);
  return source == requested ? ownVersion
                            : ownVersion + QLatin1Char('-') + QString::fromLatin1(QCryptographicHash::hash(
                                  (source + QLatin1Char('|') + FileVersion::key(source)).toUtf8(),
                                  QCryptographicHash::Sha256).toHex());
}

} // namespace ThumbnailSource

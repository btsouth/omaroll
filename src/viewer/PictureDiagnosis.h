#pragma once

#include <QString>
#include <QVariantMap>

// Why a picture could not be shown, in words a person can act on, and
// whether one that did show is cut off. Both read the file, so they run on a
// worker.
namespace PictureDiagnosis {

struct Finding {
  // missing, unreadable, empty, crashed (it crashed Omaroll before and has
  // not changed), not-picture, wrong-type (another picture format, one
  // Omaroll does not open), unsupported, damaged, truncated, too-large or
  // unknown.
  QString cause;
  QString title;
  QString detail;

  [[nodiscard]] QVariantMap toMap() const {
    return {{QStringLiteral("cause"), cause}, {QStringLiteral("title"), title},
            {QStringLiteral("detail"), detail}};
  }
};

// Reads the header and, when that is fine, decodes the picture to see what
// the reader reports. Decodes past the memory limit are not attempted.
[[nodiscard]] Finding failure(const QString& path);

// Why a video the player reported |playerError| for will not play: missing,
// unreadable, empty, not-video, unsupported (no decoder for its codec),
// truncated, damaged or unknown. Decodes its first frame with ffmpeg, under a
// time limit, when ffmpeg is installed; |ffmpeg| overrides it for tests.
[[nodiscard]] Finding videoFailure(const QString& path, const QString& playerError,
                                   const QString& ffmpeg = QString());

// A JPEG or PNG that ends before its last marker: what a download or copy
// cut short leaves behind. Qt shows the missing part as grey without saying
// so. Empty for complete files and every other format.
[[nodiscard]] QString truncation(const QString& path);

} // namespace PictureDiagnosis

#pragma once

#include <QDateTime>
#include <QSize>
#include <QString>

// One file in the library. Metadata only: omaroll never holds decoded pixels
// here, so a twenty-thousand file library costs the same to scroll as a small
// one.
struct CaptureRecord {
  enum Kind {
    Screenshot,
    Recording,
    Picture,
    Video,
    Download,
  };

  QString path;
  QString fileName;
  Kind kind = Picture;
  // Parsed out of the filename when the producer stamped one, mtime otherwise.
  QDateTime captured;
  qint64 bytes = 0;
  // mtime in milliseconds. Part of the thumbnail identity, so a file rewritten in
  // place (a recording finalised by omarchy-capture-screenrecording) gets a
  // fresh tile rather than the cached old one.
  qint64 modified = 0;
  // The medium, decided by extension. Kept apart from kind because a Download
  // can be either, and everything that plays, scrubs or trims keys off this.
  bool video = false;
  // Set from AppSettings after a scan; the scanner itself knows nothing about
  // them, so discovery stays a pure function of the filesystem.
  bool favorite = false;
  bool hidden = false;

  [[nodiscard]] bool isVideo() const { return video; }
};

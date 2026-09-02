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
  // A producer timestamp is authoritative. General photos and videos can have
  // their mtime fallback replaced later by embedded metadata without making
  // capture discovery wait for ImageMagick or ffprobe.
  bool hasProducerTimestamp = false;
  qint64 bytes = 0;
  // mtime in milliseconds. Part of the thumbnail identity, so a file rewritten in
  // place (a recording finalised by omarchy-capture-screenrecording) gets a
  // fresh tile rather than the cached old one.
  qint64 modified = 0;
  // The medium, decided by extension. Kept apart from kind because a Download
  // can be either, and everything that plays, scrubs or trims keys off this.
  bool video = false;
  // Filesystem identity, gathered by the scanner off the GUI thread so album
  // reconciliation never has to stat the whole library on it. Zero when the
  // record was not made by the scanner.
  quint64 device = 0;
  quint64 inode = 0;
  // Set from AppSettings after a scan; the scanner itself knows nothing about
  // them, so discovery stays a pure function of the filesystem.
  bool favorite = false;
  bool hidden = false;

  [[nodiscard]] bool isVideo() const { return video; }
};

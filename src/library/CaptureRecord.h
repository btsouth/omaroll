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
    Document,
  };

  QString path;
  // The discovered directory entry before media-path canonicalization.
  QString entryPath;
  QString fileName;
  // A unique RAW/JPEG sibling. Both files remain independent source rows.
  QString companionPath;
  Kind kind = Picture;
  // Parsed out of the filename when the producer stamped one, mtime otherwise.
  QDateTime captured;
  // A producer timestamp is authoritative. General photos and videos can have
  // their mtime fallback replaced later by embedded metadata without making
  // capture discovery wait for ImageMagick or ffprobe.
  bool hasProducerTimestamp = false;
  // Filled in later from embedded metadata, the same way as the date. Empty
  // for screenshots, recordings and anything without EXIF.
  QString camera;
  QString lens;
  qint64 bytes = 0;
  // mtime in milliseconds. Part of the thumbnail identity, so a file rewritten
  // in place (a recording finalised by omarchy-capture-screenrecording) gets a
  // fresh tile rather than the cached old one.
  qint64 modified = 0;
  // Full file and transcode-source identity, gathered off the GUI thread.
  QString thumbnailVersion;
  // The medium, decided by extension or an extensionless file's header.
  // Kept apart from kind because a Download
  // can be either, and everything that plays, scrubs or trims keys off this.
  bool video = false;
  // Documents currently means PDF. Kept separate from Kind because a PDF in
  // Downloads still needs document viewer behavior even if future source
  // classification grows more specific.
  bool document = false;
  // Filesystem identity, gathered by the scanner off the GUI thread so album
  // reconciliation never has to stat the whole library on it. Zero when the
  // record was not made by the scanner.
  quint64 device = 0;
  quint64 inode = 0;
  // Set from AppSettings after a scan; the scanner itself knows nothing about
  // them, so discovery stays a pure function of the filesystem.
  bool favorite = false;
  bool hidden = false;
  // Stars, 1 to 5; zero is unrated.
  int rating = 0;
  QString caption;
  // GIF/WebP use the animated image component, including extensionless files.
  bool animated = false;
  // A camera raw. Only Qt's raw plugin reads these, so the tools that open
  // the file themselves (tesseract, zbarimg, Pinta and the like) are not
  // offered for them.
  bool raw = false;

  [[nodiscard]] bool isVideo() const { return video; }
  [[nodiscard]] bool isDocument() const { return document || kind == Document; }
  [[nodiscard]] bool isRaw() const { return raw; }
};

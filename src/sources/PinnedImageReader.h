#pragma once

#include "sources/DecodeJournal.h"

#include <QByteArray>
#include <QImageReader>
#include <QList>
#include <QString>

#include <functional>

class QIODevice;

// The formats Omaroll lists, as Qt's reader names them.
namespace ImageFormats {

// The reader format for a listed image suffix ("jpe" reads as "jpeg", "dib"
// as "bmp"), or empty for anything Omaroll does not list.
[[nodiscard]] QByteArray forSuffix(const QString& suffix);

// The listed format a MIME type is read with, or empty.
[[nodiscard]] QByteArray forMimeType(const QString& mimeType);

// Every listed format an installed plugin reads, camera raws left out. The
// image fuzzer draws a file's name from these.
[[nodiscard]] const QList<QByteArray>& listed();

// Whether a .gif starts like a GIF, or a .webp like a WebP: the two formats
// the viewers play with Qt's movie reader. That reader has no format to pin,
// so a file only reaches it when its contents match its name.
[[nodiscard]] bool looksAnimated(const QString& path, const QString& suffix);

} // namespace ImageFormats

// A QImageReader that only uses decoders for formats Omaroll lists: the one
// the file's name maps to, or, when that decoder refuses the contents, the
// one for what the contents are, if that format is listed too (a PNG saved as
// .jpg). Qt's own fallback offers a file that doesn't match its name to every
// installed plugin, including about fifteen Omaroll never asks for, and one of
// those (KImageFormats' TIM reader) crashed on a 24-byte file. That fallback
// is off.
//
// A file that crashed Omaroll while it was being read is not read again until
// it changes; while the reader exists, its file is in the decode journal.
class PinnedImageReader final : public QImageReader {
public:
  explicit PinnedImageReader(const QString& path);
  // Bytes from somewhere other than a file, such as the clipboard: |format|
  // is what their source says they are, tried before their contents.
  PinnedImageReader(QIODevice* device, const QByteArray& format);

  // A listed decoder accepts the contents. canRead() says the same.
  [[nodiscard]] bool pinned() const { return m_pinned; }
  // Read as a different listed format than the name says.
  [[nodiscard]] bool renamed() const { return m_renamed; }
  // The file crashed Omaroll the last time it was read, and nothing reads it.
  [[nodiscard]] bool blocked() const { return m_blocked; }
  // What the decoder that refused the contents said, when it said anything.
  // No decoder stays attached to an unpinned reader, so every read fails.
  [[nodiscard]] QString refusal() const { return m_refusal; }

private:
  void refuse();
  void pin(const QByteArray& named, bool trustName, const std::function<QByteArray()>& sniff);

  DecodeJournal::Scope m_scope;
  bool m_pinned = false;
  bool m_renamed = false;
  bool m_blocked = false;
  QString m_refusal;
};

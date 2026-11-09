#pragma once

#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

// The library search language.
//
// Plain words match anywhere: names, picture and PDF text, captions, tags,
// albums, folders, cameras and lenses. Field tokens narrow one property:
//
//   tag:travel  album:iceland  in:pictures/2024  camera:fuji  lens:24-70
//   type:video (kind: works too)  before:2024  after:2023-06  after:2024-06-01
//   "exact phrase"  tag:"road trip"  -word  -tag:work  -"draft copy"
//
// Every term must match. Words, phrases and field values ignore case and
// accents, so "cafe" finds "Café". A field value matches the start of a word,
// so camera:fuji finds "FUJIFILM X-T4" and tag:japan finds "Travel/Japan".
// before: is earlier than the whole period and after: is later than it, so
// after:2023 before:2025 is 2024. Anything that does not parse as a field
// token, such as "note:x" or "before:soon", is searched as plain text.
namespace SearchText {

// Unicode NFKD, combining marks dropped, case folded. Returns the input
// unchanged (and shared) when there is nothing to fold.
[[nodiscard]] QString fold(const QString& text);
// As fold(), with each output character's index in the input, so a match in
// folded text can be shown in the original.
[[nodiscard]] QString fold(const QString& text, QList<qsizetype>* sourceIndexes);
// Whether needle starts at the beginning of a word anywhere in haystack.
// Both are expected folded.
[[nodiscard]] bool containsWordStart(const QString& haystack, const QString& needle);

} // namespace SearchText

struct SearchQuery {
  enum Field { Text, Tag, Album, Folder, Camera, Lens, Type, Before, After };
  enum MediaType { Screenshot, Recording, Image, Video, Download, Pdf, Raw, Animated };

  struct Term {
    Field field = Text;
    // Folded and whitespace-simplified. For Type, the MediaType as a number.
    QString value;
    bool negated = false;
    bool phrase = false;
    MediaType type = Image;
    // Before: the first day of the period. After: the last day.
    QDate date;
  };

  QList<Term> terms;

  [[nodiscard]] static SearchQuery parse(const QString& text);
  [[nodiscard]] bool isEmpty() const { return terms.isEmpty(); }
  // Whether any term needs picture or document text to decide.
  [[nodiscard]] bool hasText() const;
  // Plain words and phrases, folded, positive ones first. Used for snippets
  // and to decide whether text indexing is worth starting.
  [[nodiscard]] QStringList textValues(bool includeNegated = false) const;

  // Field names, as typed. The first of each group is the canonical one.
  [[nodiscard]] static QStringList fieldNames();
  [[nodiscard]] static int fieldFor(const QString& name);
  [[nodiscard]] static QStringList typeNames();
  [[nodiscard]] static int typeFor(const QString& name);
};

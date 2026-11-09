#pragma once

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

#include <utility>

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

// Each character on its own: Unicode NFKD, case folded, combining marks
// dropped, and the Latin letters with no decomposition spelled out (ß as ss,
// ø as o, ł as l, đ and ð as d, æ as ae, œ as oe, þ as th, ı as i, ħ as h,
// ŧ as t). Character by character, so folding a piece of text gives the same
// characters as that piece of the folded whole. Returns the input unchanged
// (and shared) when there is nothing to fold.
[[nodiscard]] QString fold(const QString& text);
// As fold(), with each output character's index in the input, so a match in
// folded text can be shown in the original.
[[nodiscard]] QString fold(const QString& text, QList<qsizetype>* sourceIndexes);

// Long text kept for search, such as a PDF's: the original and its folded
// form, both UTF-8, with a mark every couple of thousand characters where the
// two line up. A match found in the folded text is shown from the original
// by refolding only the piece around it.
struct FoldedText {
  QByteArray original;
  QByteArray folded;
  // Byte offsets in folded and original where the same character starts.
  QList<std::pair<qsizetype, qsizetype>> marks;

  [[nodiscard]] static FoldedText make(const QString& text);
  [[nodiscard]] bool isEmpty() const { return original.isEmpty(); }
  [[nodiscard]] qsizetype bytes() const { return original.size() + folded.size(); }
  // The original text around the first match of a folded needle: up to
  // |before| characters ahead of it and |after| behind, simplified, with an
  // ellipsis where it was cut. Empty when the needle is not there.
  [[nodiscard]] QString snippet(const QByteArray& needle, int before, int after) const;
};
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
    // Text: the value in UTF-8, for matching long folded text, and as typed.
    QByteArray utf8;
    QString typed;
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
  // The positive plain words as typed, which is all a saved view's "search"
  // means to 1.18 and older.
  [[nodiscard]] QString plainWords() const;
  // A search saved before field tokens existed, as a query that means the
  // same: every word is literal text, so "-draft" and "type:video" are
  // searched for as written.
  [[nodiscard]] static QString fromPlainWords(const QString& words);
  // A value as a token writes it: in quotes when it has a space or a quote,
  // with \" for a quote and \\ for a backslash inside them.
  [[nodiscard]] static QString quote(const QString& value);
  // Reads a quoted value from |text| at |from|, just past its opening quote,
  // up to the closing quote or the end. Sets |next| past what was read and
  // |closed| when the closing quote was there.
  [[nodiscard]] static QString readQuoted(const QString& text, qsizetype from, qsizetype* next,
                                          bool* closed = nullptr);

  // Field names, as typed. The first of each group is the canonical one.
  [[nodiscard]] static QStringList fieldNames();
  [[nodiscard]] static int fieldFor(const QString& name);
  [[nodiscard]] static QStringList typeNames();
  [[nodiscard]] static int typeFor(const QString& name);
};

#include "search/SearchQuery.h"

#include <QDir>
#include <QRegularExpression>

#include <algorithm>

namespace {

bool isAscii(const QString& text, bool* hasUpper) {
  *hasUpper = false;
  for (const QChar character : text) {
    const char16_t unit = character.unicode();
    if (unit >= 0x80) return false;
    if (unit >= u'A' && unit <= u'Z') *hasUpper = true;
  }
  return true;
}

struct FieldName {
  const char* name;
  SearchQuery::Field field;
  // Shown in suggestions and help; aliases still parse.
  bool listed;
};

constexpr FieldName kFields[] = {
    {"tag", SearchQuery::Tag, true},       {"tags", SearchQuery::Tag, false},
    {"album", SearchQuery::Album, true},   {"albums", SearchQuery::Album, false},
    {"in", SearchQuery::Folder, true},     {"folder", SearchQuery::Folder, false},
    {"camera", SearchQuery::Camera, true}, {"lens", SearchQuery::Lens, true},
    {"type", SearchQuery::Type, true},     {"kind", SearchQuery::Type, true},
    {"before", SearchQuery::Before, true}, {"after", SearchQuery::After, true},
};

struct TypeName {
  const char* name;
  SearchQuery::MediaType type;
  bool listed;
};

constexpr TypeName kTypes[] = {
    {"screenshot", SearchQuery::Screenshot, true}, {"screenshots", SearchQuery::Screenshot, false},
    {"shot", SearchQuery::Screenshot, false},      {"shots", SearchQuery::Screenshot, false},
    {"recording", SearchQuery::Recording, true},   {"recordings", SearchQuery::Recording, false},
    {"clip", SearchQuery::Recording, false},       {"clips", SearchQuery::Recording, false},
    {"photo", SearchQuery::Image, true},           {"photos", SearchQuery::Image, false},
    {"picture", SearchQuery::Image, false},        {"pictures", SearchQuery::Image, false},
    {"image", SearchQuery::Image, false},          {"images", SearchQuery::Image, false},
    {"still", SearchQuery::Image, false},          {"stills", SearchQuery::Image, false},
    {"video", SearchQuery::Video, true},           {"videos", SearchQuery::Video, false},
    {"download", SearchQuery::Download, true},     {"downloads", SearchQuery::Download, false},
    {"pdf", SearchQuery::Pdf, true},               {"pdfs", SearchQuery::Pdf, false},
    {"document", SearchQuery::Pdf, false},         {"documents", SearchQuery::Pdf, false},
    {"doc", SearchQuery::Pdf, false},              {"raw", SearchQuery::Raw, true},
    {"animated", SearchQuery::Animated, true},     {"gif", SearchQuery::Animated, false},
    {"animation", SearchQuery::Animated, false},
};

// YYYY, YYYY-MM or YYYY-MM-DD ("/" also separates). The period's first and
// last day.
bool parsePeriod(const QString& value, QDate* first, QDate* last) {
  static const QRegularExpression pattern(
      QStringLiteral("^(\\d{4})(?:[-/](\\d{1,2})(?:[-/](\\d{1,2}))?)?$"));
  const QRegularExpressionMatch match = pattern.match(value);
  if (!match.hasMatch()) return false;
  const int year = match.captured(1).toInt();
  if (match.captured(2).isEmpty()) {
    *first = QDate(year, 1, 1);
    *last = QDate(year, 12, 31);
  } else if (match.captured(3).isEmpty()) {
    *first = QDate(year, match.captured(2).toInt(), 1);
    *last = first->isValid() ? first->addMonths(1).addDays(-1) : QDate();
  } else {
    *first = QDate(year, match.captured(2).toInt(), match.captured(3).toInt());
    *last = *first;
  }
  return first->isValid() && last->isValid();
}

} // namespace

namespace SearchText {

namespace {

// Latin letters that NFKD leaves whole, spelled the way people type them.
// Applied after case folding, so only lower case appears here.
QLatin1StringView spelledOut(char32_t character) {
  switch (character) {
  case 0x00DF: return QLatin1StringView("ss"); // ß, and ẞ once folded
  case 0x00E6: return QLatin1StringView("ae"); // æ
  case 0x00F0: return QLatin1StringView("d");  // ð
  case 0x00F8: return QLatin1StringView("o");  // ø
  case 0x00FE: return QLatin1StringView("th"); // þ
  case 0x0111: return QLatin1StringView("d");  // đ
  case 0x0127: return QLatin1StringView("h");  // ħ
  case 0x0131: return QLatin1StringView("i");  // ı
  case 0x0142: return QLatin1StringView("l");  // ł
  case 0x0153: return QLatin1StringView("oe"); // œ
  case 0x0167: return QLatin1StringView("t");  // ŧ
  default: return {};
  }
}

// Appends one folded code point, skipping combining marks.
template <typename Sink>
void emitFolded(char32_t character, Sink&& sink) {
  if (QChar::category(character) == QChar::Mark_NonSpacing) return;
  const QLatin1StringView spelled = spelledOut(character);
  if (!spelled.isEmpty()) {
    for (const char letter : spelled) sink(QChar(QLatin1Char(letter)));
    return;
  }
  if (QChar::requiresSurrogates(character)) {
    sink(QChar(QChar::highSurrogate(character)));
    sink(QChar(QChar::lowSurrogate(character)));
  } else {
    sink(QChar(static_cast<char16_t>(character)));
  }
}

// Folds text[index, index + length), one code point, through sink. Marks are
// dropped after case folding, since folding can turn a mark into a letter
// (the Greek ypogegrammeni becomes iota).
template <typename Sink>
void foldCharacter(const QString& text, qsizetype index, qsizetype length, char32_t character,
                   Sink&& sink) {
  if (QChar::decompositionTag(character) == QChar::NoDecomposition) {
    emitFolded(QChar::toCaseFolded(character), sink);
    return;
  }
  const QString piece =
      text.mid(index, length).normalized(QString::NormalizationForm_KD).toCaseFolded();
  for (qsizetype at = 0; at < piece.size(); ++at) {
    char32_t unit = piece.at(at).unicode();
    if (piece.at(at).isHighSurrogate() && at + 1 < piece.size() &&
        piece.at(at + 1).isLowSurrogate()) {
      unit = QChar::surrogateToUcs4(piece.at(at), piece.at(at + 1));
      ++at;
    }
    emitFolded(unit, sink);
  }
}

template <typename Sink>
void foldEach(const QString& text, Sink&& sink) {
  const qsizetype size = text.size();
  const QChar* data = text.constData();
  for (qsizetype index = 0; index < size;) {
    const char16_t unit = data[index].unicode();
    if (unit < 0x80) {
      sink(index, QChar(unit >= u'A' && unit <= u'Z' ? char16_t(unit + 32) : unit));
      ++index;
      continue;
    }
    qsizetype length = 1;
    char32_t character = unit;
    if (QChar::isHighSurrogate(unit) && index + 1 < size && data[index + 1].isLowSurrogate()) {
      character = QChar::surrogateToUcs4(unit, data[index + 1].unicode());
      length = 2;
    }
    foldCharacter(text, index, length, character,
                  [&](QChar folded) { sink(index, folded); });
    index += length;
  }
}

} // namespace

QString fold(const QString& text) {
  bool upper = false;
  if (isAscii(text, &upper)) return upper ? text.toLower() : text;
  QString out;
  out.reserve(text.size());
  foldEach(text, [&](qsizetype, QChar folded) { out.append(folded); });
  return out;
}

QString fold(const QString& text, QList<qsizetype>* sourceIndexes) {
  sourceIndexes->clear();
  sourceIndexes->reserve(text.size());
  bool upper = false;
  if (isAscii(text, &upper)) {
    for (qsizetype index = 0; index < text.size(); ++index) sourceIndexes->append(index);
    return upper ? text.toLower() : text;
  }
  QString out;
  out.reserve(text.size());
  foldEach(text, [&](qsizetype source, QChar folded) {
    out.append(folded);
    sourceIndexes->append(source);
  });
  return out;
}

FoldedText FoldedText::make(const QString& text) {
  // Folding is character by character, so pieces fold the same alone as in
  // the whole. A piece never splits a surrogate pair, so its UTF-8 is the
  // same piece of the whole's UTF-8 too.
  constexpr qsizetype kPiece = 2048;
  FoldedText result;
  result.original.reserve(text.size());
  result.folded.reserve(text.size());
  for (qsizetype at = 0; at < text.size();) {
    qsizetype end = std::min(text.size(), at + kPiece);
    if (end < text.size() && text.at(end - 1).isHighSurrogate() && text.at(end).isLowSurrogate()) {
      ++end;
    }
    const QString piece = text.mid(at, end - at);
    result.marks.append({result.folded.size(), result.original.size()});
    result.original += piece.toUtf8();
    result.folded += fold(piece).toUtf8();
    at = end;
  }
  result.original.squeeze();
  result.folded.squeeze();
  return result;
}

QString FoldedText::snippet(const QByteArray& needle, int before, int after) const {
  if (needle.isEmpty() || marks.isEmpty()) return {};
  const qsizetype found = folded.indexOf(needle);
  if (found < 0) return {};
  const qsizetype foundEnd = found + needle.size();
  // The piece where the match starts, and the one where it ends.
  const auto byFolded = [](qsizetype offset, const std::pair<qsizetype, qsizetype>& mark) {
    return offset < mark.first;
  };
  const qsizetype first = std::upper_bound(marks.cbegin(), marks.cend(), found, byFolded) -
                          marks.cbegin() - 1;
  const qsizetype last = std::upper_bound(marks.cbegin(), marks.cend(), foundEnd - 1, byFolded) -
                         marks.cbegin() - 1;
  if (first < 0 || last < first) return {};
  const qsizetype rawStart = marks.at(first).second;
  const qsizetype rawEnd = last + 1 < marks.size() ? marks.at(last + 1).second : original.size();
  const QString window = QString::fromUtf8(original.constData() + rawStart, rawEnd - rawStart);
  QList<qsizetype> sources;
  const QString foldedWindow = fold(window, &sources);
  const qsizetype local =
      QString::fromUtf8(folded.constData() + marks.at(first).first, found - marks.at(first).first)
          .size();
  const QString term = QString::fromUtf8(needle);
  if (local + term.size() > foldedWindow.size() ||
      QStringView(foldedWindow).mid(local, term.size()) != term) {
    return {};
  }

  // A little of the original either side, cut at a character boundary.
  const auto continuation = [this](qsizetype offset) {
    return (static_cast<unsigned char>(original.at(offset)) & 0xC0) == 0x80;
  };
  qsizetype prefixStart = std::max<qsizetype>(0, rawStart - qsizetype(before) * 4);
  while (prefixStart < rawStart && continuation(prefixStart)) ++prefixStart;
  qsizetype suffixEnd = std::min<qsizetype>(original.size(), rawEnd + qsizetype(after) * 4);
  while (suffixEnd > rawEnd && suffixEnd < original.size() && continuation(suffixEnd)) --suffixEnd;
  const QString prefix =
      QString::fromUtf8(original.constData() + prefixStart, rawStart - prefixStart);
  const QString text = prefix + window +
                       QString::fromUtf8(original.constData() + rawEnd, suffixEnd - rawEnd);

  // Each end on a character, never between the halves of a surrogate pair:
  // the match's last character whole, and the context widened rather than cut.
  const auto splitsPair = [&text](qsizetype at) {
    return at > 0 && at < text.size() && text.at(at).isLowSurrogate() &&
           text.at(at - 1).isHighSurrogate();
  };
  const qsizetype match = prefix.size() + sources.at(local);
  qsizetype matchEnd = prefix.size() + sources.at(local + term.size() - 1) + 1;
  if (splitsPair(matchEnd)) ++matchEnd;
  qsizetype start = std::max<qsizetype>(0, match - before);
  if (splitsPair(start)) --start;
  qsizetype end = std::min<qsizetype>(text.size(), matchEnd + after);
  if (splitsPair(end)) ++end;
  QString snippet = text.mid(start, end - start).simplified();
  if (start > 0 || prefixStart > 0) snippet.prepend(QChar(0x2026));
  if (end < text.size() || suffixEnd < original.size()) snippet.append(QChar(0x2026));
  return snippet;
}

bool containsWordStart(const QString& haystack, const QString& needle) {
  if (needle.isEmpty()) return true;
  qsizetype from = 0;
  while (true) {
    const qsizetype at = haystack.indexOf(needle, from);
    if (at < 0) return false;
    if (at == 0 || !haystack.at(at - 1).isLetterOrNumber()) return true;
    from = at + 1;
  }
}

} // namespace SearchText

QStringList SearchQuery::fieldNames() {
  QStringList names;
  for (const FieldName& field : kFields) {
    if (field.listed) names.append(QLatin1String(field.name));
  }
  return names;
}

int SearchQuery::fieldFor(const QString& name) {
  const QString folded = name.toLower();
  for (const FieldName& field : kFields) {
    if (folded == QLatin1String(field.name)) return field.field;
  }
  return -1;
}

QStringList SearchQuery::typeNames() {
  QStringList names;
  for (const TypeName& type : kTypes) {
    if (type.listed) names.append(QLatin1String(type.name));
  }
  return names;
}

int SearchQuery::typeFor(const QString& name) {
  const QString folded = SearchText::fold(name);
  for (const TypeName& type : kTypes) {
    if (folded == QLatin1String(type.name)) return type.type;
  }
  return -1;
}

SearchQuery SearchQuery::parse(const QString& text) {
  SearchQuery query;
  const qsizetype size = text.size();
  qsizetype at = 0;
  const auto readQuoted = [&](qsizetype* position) {
    // An unclosed quote runs to the end, which is what someone still typing
    // means.
    return SearchQuery::readQuoted(text, *position, position);
  };
  const auto addText = [&](const QString& raw, bool negated, bool phrase) {
    const QString value = SearchText::fold(raw.simplified());
    if (value.isEmpty() || value == QStringLiteral("-")) return;
    Term term;
    term.value = value;
    term.utf8 = value.toUtf8();
    term.typed = raw.simplified();
    term.negated = negated;
    term.phrase = phrase || value.contains(QLatin1Char(' '));
    query.terms.append(term);
  };

  while (at < size) {
    while (at < size && text.at(at).isSpace()) ++at;
    if (at >= size) break;
    bool negated = false;
    if (text.at(at) == QLatin1Char('-') && at + 1 < size && !text.at(at + 1).isSpace()) {
      negated = true;
      ++at;
    }
    if (text.at(at) == QLatin1Char('"')) {
      ++at;
      addText(readQuoted(&at), negated, true);
      continue;
    }
    const qsizetype start = at;
    QString name;
    QString value;
    bool quotedValue = false;
    while (at < size && !text.at(at).isSpace()) {
      if (text.at(at) == QLatin1Char(':') && name.isEmpty() && at > start) {
        name = text.mid(start, at - start);
        if (at + 1 < size && text.at(at + 1) == QLatin1Char('"')) {
          at += 2;
          value = readQuoted(&at);
          quotedValue = true;
          break;
        }
      }
      ++at;
    }
    const QString raw = quotedValue ? name + QStringLiteral(":") + value
                                    : text.mid(start, at - start);
    if (!quotedValue && !name.isEmpty()) value = raw.mid(name.size() + 1);

    const int field = name.isEmpty() ? -1 : fieldFor(name);
    if (field < 0) {
      addText(raw, negated, quotedValue);
      continue;
    }
    const QString simplified = value.simplified();
    if (simplified.isEmpty()) {
      // "tag:" while the value is still being typed narrows nothing yet.
      continue;
    }
    Term term;
    term.field = static_cast<Field>(field);
    term.negated = negated;
    term.phrase = quotedValue;
    if (field == Type) {
      const int type = typeFor(simplified);
      if (type < 0) {
        addText(raw, negated, quotedValue);
        continue;
      }
      term.type = static_cast<MediaType>(type);
      term.value = QString::number(type);
    } else if (field == Before || field == After) {
      QDate first;
      QDate last;
      if (!parsePeriod(simplified, &first, &last)) {
        addText(raw, negated, quotedValue);
        continue;
      }
      term.date = field == Before ? first : last;
      term.value = simplified;
    } else {
      term.value = SearchText::fold(simplified);
      // Folders are matched as written from home ("~/Pictures/2024"), so a
      // full path under home is rewritten the same way.
      if (field == Folder) {
        const QString home = SearchText::fold(QDir::homePath());
        if (term.value == home || term.value == home + QLatin1Char('/')) {
          term.value = QStringLiteral("~");
        } else if (term.value.startsWith(home + QLatin1Char('/'))) {
          term.value = QStringLiteral("~/") + term.value.mid(home.size() + 1);
        }
      }
    }
    query.terms.append(term);
  }
  return query;
}

bool SearchQuery::hasText() const {
  for (const Term& term : terms) {
    if (term.field == Text) return true;
  }
  return false;
}

QStringList SearchQuery::textValues(bool includeNegated) const {
  QStringList values;
  for (const Term& term : terms) {
    if (term.field == Text && !term.negated) values.append(term.value);
  }
  if (includeNegated) {
    for (const Term& term : terms) {
      if (term.field == Text && term.negated) values.append(term.value);
    }
  }
  return values;
}

QString SearchQuery::plainWords() const {
  QStringList words;
  for (const Term& term : terms) {
    if (term.field == Text && !term.negated) words.append(term.typed);
  }
  return words.join(QLatin1Char(' '));
}

QString SearchQuery::fromPlainWords(const QString& words) {
  QStringList parts;
  for (QString word : words.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
    if (word.startsWith(QLatin1Char('-')) || word.contains(QLatin1Char(':')) ||
        word.contains(QLatin1Char('"'))) {
      // Quoted, quotes and all, so the word is searched for as written.
      word = QLatin1Char('"') +
             word.replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
                 .replace(QLatin1Char('"'), QStringLiteral("\\\"")) +
             QLatin1Char('"');
    }
    parts.append(word);
  }
  return parts.join(QLatin1Char(' '));
}

QString SearchQuery::quote(const QString& value) {
  if (!value.contains(QLatin1Char(' ')) && !value.contains(QLatin1Char('\t')) &&
      !value.contains(QLatin1Char('"'))) {
    return value;
  }
  QString escaped = value;
  escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
  return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString SearchQuery::readQuoted(const QString& text, qsizetype from, qsizetype* next,
                                bool* closed) {
  QString value;
  qsizetype at = from;
  if (closed) *closed = false;
  while (at < text.size()) {
    const QChar character = text.at(at);
    if (character == QLatin1Char('\\') && at + 1 < text.size() &&
        (text.at(at + 1) == QLatin1Char('"') || text.at(at + 1) == QLatin1Char('\\'))) {
      value.append(text.at(at + 1));
      at += 2;
      continue;
    }
    if (character == QLatin1Char('"')) {
      if (closed) *closed = true;
      ++at;
      break;
    }
    value.append(character);
    ++at;
  }
  if (next) *next = at;
  return value;
}

#include "search/SearchQuery.h"

#include <QDir>
#include <QRegularExpression>

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

bool dropped(QChar character) { return character.category() == QChar::Mark_NonSpacing; }

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

QString fold(const QString& text) {
  bool upper = false;
  if (isAscii(text, &upper)) return upper ? text.toLower() : text;
  QString decomposed = text.normalized(QString::NormalizationForm_KD);
  decomposed.removeIf(dropped);
  return decomposed.toCaseFolded();
}

QString fold(const QString& text, QList<qsizetype>* sourceIndexes) {
  sourceIndexes->clear();
  bool upper = false;
  if (isAscii(text, &upper)) {
    sourceIndexes->reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index) sourceIndexes->append(index);
    return upper ? text.toLower() : text;
  }
  QString out;
  out.reserve(text.size());
  sourceIndexes->reserve(text.size());
  for (qsizetype index = 0; index < text.size(); ++index) {
    const qsizetype length =
        text.at(index).isHighSurrogate() && index + 1 < text.size() &&
                text.at(index + 1).isLowSurrogate()
            ? 2 : 1;
    const QString piece =
        text.mid(index, length).normalized(QString::NormalizationForm_KD).toCaseFolded();
    for (const QChar character : piece) {
      if (dropped(character)) continue;
      out.append(character);
      sourceIndexes->append(index);
    }
    index += length - 1;
  }
  return out;
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
    // *position is just past the opening quote. An unclosed quote runs to
    // the end, which is what someone still typing means.
    const qsizetype close = text.indexOf(QLatin1Char('"'), *position);
    const qsizetype end = close < 0 ? size : close;
    const QString value = text.mid(*position, end - *position);
    *position = close < 0 ? size : close + 1;
    return value;
  };
  const auto addText = [&](const QString& raw, bool negated, bool phrase) {
    const QString value = SearchText::fold(raw.simplified());
    if (value.isEmpty() || value == QStringLiteral("-")) return;
    Term term;
    term.value = value;
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
        const QString home = SearchText::fold(QDir::homePath()) + QLatin1Char('/');
        if (term.value.startsWith(home)) term.value = QStringLiteral("~/") + term.value.mid(home.size());
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

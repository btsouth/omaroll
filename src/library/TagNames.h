#pragma once

#include <QString>
#include <QStringList>

// Tag names nest with a slash: "Travel/Japan" sits under "Travel". Shared by
// the organization settings and the reader for keywords stored in files, so a
// keyword from a file becomes exactly the tag a user would have typed.
namespace TagNames {

// "travel / japan /" becomes "travel/japan". Empty segments are dropped, so
// a stray slash cannot create a nameless level.
inline QString normalized(const QString& name) {
  QStringList segments;
  for (const QString& part : name.split(QLatin1Char('/'))) {
    const QString segment = part.simplified();
    if (!segment.isEmpty()) segments.append(segment);
  }
  return segments.join(QLatin1Char('/')).left(80);
}

// Natural order ignoring case: runs of ASCII digits compare as numbers, so
// "Tag 2" sorts before "Tag 10". Only '0' to '9' make a number: other
// scripts' digits compare as characters, which keeps the order consistent
// (a digit run sorts where its first digit's character would). Ties fall
// back to the plain comparison.
inline bool isAsciiDigit(QChar c) { return c.unicode() >= u'0' && c.unicode() <= u'9'; }
inline int naturalCompare(QStringView a, QStringView b) {
  qsizetype i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (isAsciiDigit(a.at(i)) && isAsciiDigit(b.at(j))) {
      qsizetype endA = i, endB = j;
      while (endA < a.size() && isAsciiDigit(a.at(endA))) ++endA;
      while (endB < b.size() && isAsciiDigit(b.at(endB))) ++endB;
      // Leading zeros do not make a number larger.
      qsizetype startA = i, startB = j;
      while (startA + 1 < endA && a.at(startA) == u'0') ++startA;
      while (startB + 1 < endB && b.at(startB) == u'0') ++startB;
      if (endA - startA != endB - startB) return endA - startA < endB - startB ? -1 : 1;
      for (qsizetype k = 0; k < endA - startA; ++k) {
        const char16_t da = a.at(startA + k).unicode(), db = b.at(startB + k).unicode();
        if (da != db) return da < db ? -1 : 1;
      }
      i = endA;
      j = endB;
      continue;
    }
    const QChar ca = a.at(i).toCaseFolded(), cb = b.at(j).toCaseFolded();
    if (ca != cb) return ca < cb ? -1 : 1;
    ++i;
    ++j;
  }
  if (i < a.size() || j < b.size()) return i < a.size() ? 1 : -1;
  return a.compare(b, Qt::CaseInsensitive);
}

// Tree order: each level in natural order ignoring case, a parent before
// its children.
inline bool lessThan(const QString& first, const QString& second) {
  const QStringList a = first.split(QLatin1Char('/'));
  const QStringList b = second.split(QLatin1Char('/'));
  for (qsizetype index = 0; index < qMin(a.size(), b.size()); ++index) {
    if (const int order = naturalCompare(a.at(index), b.at(index)); order != 0) {
      return order < 0;
    }
  }
  return a.size() < b.size();
}

// |candidate| is nested somewhere below |parent|, compared as written.
inline bool isUnder(const QString& candidate, const QString& parent) {
  return candidate.size() > parent.size() && candidate.startsWith(parent) &&
         candidate.at(parent.size()) == QLatin1Char('/');
}

// The same ignoring case, for keywords from files, whose spelling may differ
// from the local tag's.
inline bool isUnderIgnoringCase(const QString& candidate, const QString& parent) {
  return candidate.size() > parent.size() &&
         candidate.startsWith(parent, Qt::CaseInsensitive) &&
         candidate.at(parent.size()) == QLatin1Char('/');
}

// The key two spellings of one tag share.
inline QString folded(const QString& name) { return name.toCaseFolded(); }

} // namespace TagNames

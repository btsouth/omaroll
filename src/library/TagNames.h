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

// Tree order: each level compared ignoring case, a parent before its children.
inline bool lessThan(const QString& first, const QString& second) {
  const QStringList a = first.split(QLatin1Char('/'));
  const QStringList b = second.split(QLatin1Char('/'));
  for (qsizetype index = 0; index < qMin(a.size(), b.size()); ++index) {
    if (const int order = a.at(index).compare(b.at(index), Qt::CaseInsensitive); order != 0) {
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

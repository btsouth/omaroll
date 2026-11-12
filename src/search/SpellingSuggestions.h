#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <atomic>

// "Did you mean": for a search that found nothing, the same search with each
// unknown word replaced by the closest word the library knows, such as
// "invoice" for "invoce". Known words come from whatever the caller gathers
// (names, folders, tags, albums, cameras, captions, recognized text), so new
// sources only add text. Matching is case and accent blind, as search is.
namespace SpellingSuggestions {

// Folded words of three or more letters or digits, and how often each was
// seen.
using Vocabulary = QHash<QString, int>;

// Text gathered for the vocabulary: as written, and already folded (UTF-8),
// as the search index keeps picture text.
struct Texts {
  QStringList plain;
  QList<QByteArray> folded;
};

void addWords(const QString& text, Vocabulary* vocabulary);
void addFoldedWords(const QByteArray& folded, Vocabulary* vocabulary);
[[nodiscard]] Vocabulary vocabularyOf(const Texts& texts,
                                      const std::atomic_bool* cancel = nullptr);

// Edits between two words, counting a swap of neighbours as one, or
// limit + 1 once it is more than limit.
[[nodiscard]] int distance(QStringView left, QStringView right, int limit);
// The known word closest to a folded word: one edit for four or five
// letters, two for longer, none for shorter. Ties go to the more common
// word, then the first alphabetically. Empty when the word is known or
// nothing is close.
[[nodiscard]] QString closest(const QString& word, const Vocabulary& vocabulary);
// The query with its unknown plain words replaced, field tokens, phrases and
// exclusions left as typed. Empty when no word changes.
[[nodiscard]] QString suggest(const QString& query, const Vocabulary& vocabulary);

} // namespace SpellingSuggestions

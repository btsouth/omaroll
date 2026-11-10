#include "search/SpellingSuggestions.h"

#include "search/SearchQuery.h"

#include <QRegularExpression>

#include <algorithm>
#include <vector>

namespace SpellingSuggestions {
namespace {

constexpr qsizetype kShortestWord = 3;
constexpr qsizetype kLongestWord = 32;
// Enough for a large library's names and text; past it, new words are not
// added, only counted.
constexpr qsizetype kMaximumWords = 200'000;

template <typename Add>
void eachWord(QStringView text, Add add) {
  qsizetype start = -1;
  for (qsizetype at = 0; at <= text.size(); ++at) {
    const bool letter = at < text.size() && text.at(at).isLetterOrNumber();
    if (letter && start < 0) start = at;
    if (!letter && start >= 0) {
      const qsizetype length = at - start;
      if (length >= kShortestWord && length <= kLongestWord) add(text.mid(start, length));
      start = -1;
    }
  }
}

void count(QStringView word, Vocabulary* vocabulary) {
  const auto found = vocabulary->find(word.toString());
  if (found != vocabulary->end()) {
    ++found.value();
  } else if (vocabulary->size() < kMaximumWords) {
    vocabulary->insert(word.toString(), 1);
  }
}

} // namespace

void addWords(const QString& text, Vocabulary* vocabulary) {
  if (text.isEmpty()) return;
  const QString folded = SearchText::fold(text);
  eachWord(folded, [vocabulary](QStringView word) { count(word, vocabulary); });
}

void addFoldedWords(const QByteArray& folded, Vocabulary* vocabulary) {
  if (folded.isEmpty()) return;
  const QString text = QString::fromUtf8(folded);
  eachWord(text, [vocabulary](QStringView word) { count(word, vocabulary); });
}

Vocabulary vocabularyOf(const Texts& texts, const std::atomic_bool* cancel) {
  Vocabulary vocabulary;
  for (const QString& text : texts.plain) {
    if (cancel && cancel->load()) return {};
    addWords(text, &vocabulary);
  }
  for (const QByteArray& text : texts.folded) {
    if (cancel && cancel->load()) return {};
    addFoldedWords(text, &vocabulary);
  }
  return vocabulary;
}

int distance(QStringView left, QStringView right, int limit) {
  const qsizetype rows = left.size();
  const qsizetype columns = right.size();
  if (std::abs(rows - columns) > limit) return limit + 1;
  // Optimal string alignment over three rows.
  std::vector<int> before(std::size_t(columns + 1));
  std::vector<int> previous(std::size_t(columns + 1));
  std::vector<int> current(std::size_t(columns + 1));
  for (qsizetype j = 0; j <= columns; ++j) previous[std::size_t(j)] = int(j);
  for (qsizetype i = 1; i <= rows; ++i) {
    current[0] = int(i);
    int smallest = current[0];
    for (qsizetype j = 1; j <= columns; ++j) {
      const int cost = left.at(i - 1) == right.at(j - 1) ? 0 : 1;
      int value = std::min({previous[std::size_t(j)] + 1, current[std::size_t(j - 1)] + 1,
                            previous[std::size_t(j - 1)] + cost});
      if (i > 1 && j > 1 && left.at(i - 1) == right.at(j - 2) &&
          left.at(i - 2) == right.at(j - 1)) {
        value = std::min(value, before[std::size_t(j - 2)] + 1);
      }
      current[std::size_t(j)] = value;
      smallest = std::min(smallest, value);
    }
    if (smallest > limit) return limit + 1;
    std::swap(before, previous);
    std::swap(previous, current);
  }
  return std::min(previous[std::size_t(columns)], limit + 1);
}

QString closest(const QString& word, const Vocabulary& vocabulary) {
  const qsizetype length = word.size();
  if (length < 4 || length > kLongestWord || vocabulary.contains(word)) return {};
  const int limit = length <= 5 ? 1 : 2;
  QString best;
  int bestDistance = limit + 1;
  int bestCount = 0;
  for (auto it = vocabulary.cbegin(); it != vocabulary.cend(); ++it) {
    const QString& known = it.key();
    if (std::abs(known.size() - length) > limit) continue;
    const int edits = distance(word, known, std::min(limit, bestDistance));
    if (edits > limit) continue;
    if (edits < bestDistance || (edits == bestDistance && it.value() > bestCount) ||
        (edits == bestDistance && it.value() == bestCount && known < best)) {
      best = known;
      bestDistance = edits;
      bestCount = it.value();
    }
  }
  return best;
}

QString suggest(const QString& query, const Vocabulary& vocabulary) {
  if (vocabulary.isEmpty()) return {};
  // Tokens and the space between them, so the rest stays as typed.
  static const QRegularExpression pieces(QStringLiteral("\\S+|\\s+"));
  QString result;
  bool changed = false;
  bool quoted = false;
  for (auto it = pieces.globalMatch(query); it.hasNext();) {
    const QString piece = it.next().captured();
    const bool plain = !quoted && !piece.front().isSpace() && piece.front() != u'-' &&
                       !piece.contains(u'"') && !piece.contains(u':');
    if (piece.count(u'"') % 2 == 1) quoted = !quoted;
    if (!plain) {
      result += piece;
      continue;
    }
    const QString word = SearchText::fold(piece);
    const QString replacement = closest(word, vocabulary);
    if (replacement.isEmpty()) {
      result += piece;
    } else {
      result += replacement;
      changed = true;
    }
  }
  return changed ? result : QString();
}

} // namespace SpellingSuggestions

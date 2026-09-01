#include "app/AppSettings.h"

#include <QCoreApplication>

namespace {
constexpr auto kFavorites = "library/favorites";
constexpr auto kHidden = "library/hidden";
constexpr auto kShowHidden = "library/showHidden";
constexpr auto kSortMode = "library/sortMode";
constexpr auto kKindFilter = "library/kindFilter";
constexpr auto kScanDownloads = "sources/scanDownloads";
constexpr auto kRecursionDepth = "sources/recursionDepth";
} // namespace

AppSettings::AppSettings(QObject* parent)
    : QObject(parent),
      m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("omaroll"),
                 QStringLiteral("omaroll")) {
  const QStringList favorites = m_settings.value(kFavorites).toStringList();
  m_favorites = QSet<QString>(favorites.begin(), favorites.end());

  const QStringList hidden = m_settings.value(kHidden).toStringList();
  m_hidden = QSet<QString>(hidden.begin(), hidden.end());

  m_showHidden = m_settings.value(kShowHidden, false).toBool();
  m_sortMode = m_settings.value(kSortMode, 0).toInt();
  m_kindFilter = m_settings.value(kKindFilter, -1).toInt();
  m_scanDownloads = m_settings.value(kScanDownloads, true).toBool();
  m_recursionDepth = qBound(1, m_settings.value(kRecursionDepth, 4).toInt(), 8);
}

void AppSettings::setShowHidden(bool value) {
  if (m_showHidden == value) {
    return;
  }
  m_showHidden = value;
  m_settings.setValue(kShowHidden, value);
  emit showHiddenChanged();
}

void AppSettings::setSortMode(int value) {
  if (m_sortMode == value) {
    return;
  }
  m_sortMode = value;
  m_settings.setValue(kSortMode, value);
  emit sortModeChanged();
}

void AppSettings::setKindFilter(int value) {
  if (m_kindFilter == value) {
    return;
  }
  m_kindFilter = value;
  m_settings.setValue(kKindFilter, value);
  emit kindFilterChanged();
}

void AppSettings::setScanDownloads(bool value) {
  if (m_scanDownloads == value) {
    return;
  }
  m_scanDownloads = value;
  m_settings.setValue(kScanDownloads, value);
  emit scanDownloadsChanged();
}

void AppSettings::setRecursionDepth(int value) {
  const int bounded = qBound(1, value, 8);
  if (m_recursionDepth == bounded) {
    return;
  }
  m_recursionDepth = bounded;
  m_settings.setValue(kRecursionDepth, bounded);
  emit recursionDepthChanged();
}

bool AppSettings::isFavorite(const QString& path) const { return m_favorites.contains(path); }
bool AppSettings::isHidden(const QString& path) const { return m_hidden.contains(path); }

void AppSettings::toggleFavorite(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  if (!m_favorites.remove(path)) {
    m_favorites.insert(path);
  }
  persistMarks();
  emit marksChanged();
}

void AppSettings::toggleHidden(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  if (!m_hidden.remove(path)) {
    m_hidden.insert(path);
  }
  persistMarks();
  emit marksChanged();
}

void AppSettings::pruneMissing(const QSet<QString>& livePaths) {
  const qsizetype before = m_favorites.size() + m_hidden.size();
  m_favorites.intersect(livePaths);
  m_hidden.intersect(livePaths);
  if (m_favorites.size() + m_hidden.size() != before) {
    persistMarks();
  }
}

void AppSettings::persistMarks() {
  m_settings.setValue(kFavorites, QStringList(m_favorites.begin(), m_favorites.end()));
  m_settings.setValue(kHidden, QStringList(m_hidden.begin(), m_hidden.end()));
}
